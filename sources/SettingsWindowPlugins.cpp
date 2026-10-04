#include "SettingsWindow.h"

#include <QHeaderView>
#include <QLabel>
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

    if (pluginsPageAdded) {
        return;
    }

    MainWindow* mainWindow = owningMainWindow(parentWidget());
    QTabWidget* tabs = settingsTabs(*this);
    if (!mainWindow || !tabs) {
        return;
    }

    pluginsPageAdded = true;
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

    connect(tree, &QTreeWidget::itemChanged, tree,
            [&manager, tree](QTreeWidgetItem* item, int column) {
        if (!item || column != 0) {
            return;
        }
        const QString pluginId = item->data(0, Qt::UserRole).toString();
        if (pluginId.isEmpty()) {
            return;
        }

        const bool requested = item->checkState(0) == Qt::Checked;
        if (manager.setPluginEnabled(pluginId, requested)) {
            return;
        }

        const QSignalBlocker blocker(tree);
        item->setCheckState(0, requested ? Qt::Unchecked : Qt::Checked);
        item->setToolTip(0, item->toolTip(0)
            + QObject::tr("\nFailed to change plugin state."));
    });

    layout->addWidget(tree, 1);

    auto* paths = new QLabel(
        tr("Plugin folders:\n%1").arg(manager.pluginSearchPaths().join(QLatin1Char('\n'))),
        page);
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    paths->setWordWrap(true);
    layout->addWidget(paths);

    tabs->addTab(page, tr("Plugins"));
}

} // namespace Mattermost
