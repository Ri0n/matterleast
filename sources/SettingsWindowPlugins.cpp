#include "SettingsWindow.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "mainwindow.h"
#include "plugins/PluginManager.h"

namespace Mattermost {
namespace {

MainWindow* owningMainWindow(QWidget* widget)
{
    QWidget* current = widget;
    while (current) {
        if (auto* mainWindow = qobject_cast<MainWindow*>(current)) {
            return mainWindow;
        }
        current = current->parentWidget();
    }
    return nullptr;
}

QTabWidget* settingsTabs(SettingsWindow& window)
{
    const auto tabs = window.findChildren<QTabWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    return tabs.isEmpty() ? nullptr : tabs.front();
}

} // namespace

void SettingsWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    if (pluginsPageAdded_) {
        return;
    }

    MainWindow* mainWindow = owningMainWindow(parentWidget());
    QTabWidget* tabs = settingsTabs(*this);
    if (!mainWindow || !tabs) {
        return;
    }

    pluginsPageAdded_ = true;
    auto& manager = PluginManager::instance(mainWindow->backendInstance());

    auto* page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* description = new QLabel(
        tr("Native plugins run inside MatterLeast and are not sandboxed. "
           "Only enable plugins you trust."), page);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* tree = new QTreeWidget(page);
    tree->setObjectName(QStringLiteral("pluginsSettingsTree"));
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Plugin"), tr("Version")});
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    const QList<PluginInfo> plugins = manager.pluginInfos();
    for (const PluginInfo& plugin : plugins) {
        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, plugin.name.isEmpty() ? plugin.id : plugin.name);
        item->setText(1, plugin.version);
        item->setData(0, Qt::UserRole, plugin.id);
        item->setToolTip(0,
            tr("ID: %1\n%2").arg(plugin.id, plugin.filePath));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, plugin.enabled ? Qt::Checked : Qt::Unchecked);
    }

    if (plugins.isEmpty()) {
        auto* empty = new QTreeWidgetItem(tree);
        empty->setText(0, tr("No plugins found"));
        empty->setFlags(Qt::ItemIsEnabled);
    }

    auto* settingsGroup = new QGroupBox(tr("Plugin settings"), page);
    settingsGroup->setObjectName(QStringLiteral("pluginSettingsGroup"));
    auto* settingsLayout = new QVBoxLayout(settingsGroup);
    settingsLayout->setSpacing(8);
    settingsGroup->hide();

    // The extension publishes typed descriptors; the host owns all widgets
    // and persistent options. Plugins do not receive UI or application types.
    const auto rebuildSettings = [&manager, tree, settingsGroup, settingsLayout] {
        while (QLayoutItem* item = settingsLayout->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                delete widget;
            }
            delete item;
        }

        QTreeWidgetItem* selected = tree->currentItem();
        const QString id = selected
            ? selected->data(0, Qt::UserRole).toString() : QString();
        const auto settings = manager.pluginSettings(id);
        settingsGroup->setVisible(!settings.isEmpty());

        for (const QVariantHash& descriptor : settings) {
            const QString key = descriptor.value(QStringLiteral("key")).toString();
            const QString type = descriptor.value(QStringLiteral("type")).toString();
            auto* row = new QWidget(settingsGroup);
            auto* rowLayout = new QVBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(3);

            const QString title = descriptor.value(QStringLiteral("title")).toString();
            if (type == QLatin1String("integer")) {
                auto* controls = new QHBoxLayout;
                auto* label = new QLabel(title, row);
                auto* edit = new QSpinBox(row);
                edit->setObjectName(QStringLiteral("pluginSetting_") + key);
                edit->setRange(descriptor.value(QStringLiteral("minimum")).toInt(),
                               descriptor.value(QStringLiteral("maximum")).toInt());
                edit->setSingleStep(qMax(1,
                    descriptor.value(QStringLiteral("step"), 1).toInt()));
                edit->setSuffix(descriptor.value(QStringLiteral("suffix")).toString());
                const int initial = descriptor.value(QStringLiteral("value")).toInt();
                edit->setValue(initial);
                controls->addWidget(label);
                controls->addStretch(1);
                controls->addWidget(edit);
                rowLayout->addLayout(controls);
                QObject::connect(edit, qOverload<int>(&QSpinBox::valueChanged), row,
                    [&manager, id, key, edit, previous = initial](int value) mutable {
                    if (manager.setPluginSetting(id, key, value)) {
                        previous = value;
                    } else {
                        const QSignalBlocker blocker(edit);
                        edit->setValue(previous);
                    }
                });
            } else if (type == QLatin1String("boolean")) {
                auto* edit = new QCheckBox(title, row);
                edit->setObjectName(QStringLiteral("pluginSetting_") + key);
                const bool initial = descriptor.value(QStringLiteral("value")).toBool();
                edit->setChecked(initial);
                rowLayout->addWidget(edit);
                QObject::connect(edit, &QCheckBox::toggled, row,
                    [&manager, id, key, edit, previous = initial](bool value) mutable {
                    if (manager.setPluginSetting(id, key, value)) {
                        previous = value;
                    } else {
                        const QSignalBlocker blocker(edit);
                        edit->setChecked(previous);
                    }
                });
            }

            const QString description =
                descriptor.value(QStringLiteral("description")).toString();
            if (!description.isEmpty()) {
                auto* explanation = new QLabel(description, row);
                explanation->setWordWrap(true);
                explanation->setTextFormat(Qt::PlainText);
                rowLayout->addWidget(explanation);
            }
            settingsLayout->addWidget(row);
        }
    };

    connect(tree, &QTreeWidget::currentItemChanged, settingsGroup,
            [rebuildSettings](QTreeWidgetItem*, QTreeWidgetItem*) {
        rebuildSettings();
    });

    connect(tree, &QTreeWidget::itemChanged, tree,
            [&manager, tree, rebuildSettings](QTreeWidgetItem* item, int column) {
        if (!item || column != 0) {
            return;
        }
        const QString pluginId = item->data(0, Qt::UserRole).toString();
        if (pluginId.isEmpty()) {
            return;
        }

        const bool requested = item->checkState(0) == Qt::Checked;
        if (manager.setPluginEnabled(pluginId, requested)) {
            rebuildSettings();
            return;
        }

        const QSignalBlocker blocker(tree);
        item->setCheckState(0, requested ? Qt::Unchecked : Qt::Checked);
        item->setToolTip(0, item->toolTip(0)
            + QObject::tr("\nFailed to change plugin state."));
    });

    layout->addWidget(tree, 1);
    layout->addWidget(settingsGroup);
    if (!plugins.isEmpty()) {
        tree->setCurrentItem(tree->topLevelItem(0));
    }

    auto* paths = new QLabel(
        tr("Plugin folders:\n%1").arg(manager.pluginSearchPaths().join(QLatin1Char('\n'))),
        page);
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    paths->setWordWrap(true);
    layout->addWidget(paths);

    tabs->addTab(page, tr("Plugins"));
}

} // namespace Mattermost
