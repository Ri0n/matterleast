#include <algorithm>

#include <QAbstractAnimation>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include "backend/Backend.h"
#include "chat-area/post/PostWidget.h"
#include "reactions/ReactionUsageTracker.h"
#include "ui/RankedEmojiPresentation.h"

namespace Mattermost {

class ReactionQuickBarController : public QObject
{
public:
    static ReactionQuickBarController& instance()
    {
        static ReactionQuickBarController controller;
        return controller;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        const QEvent::Type type = event->type();

        if (activePost_ && watched == activePost_->hoverActions_) {
            if (type == QEvent::Hide || type == QEvent::Destroy) {
                clearQuickBar();
            }
            return QObject::eventFilter(watched, event);
        }

        QWidget* watchedWidget = qobject_cast<QWidget*>(watched);
        if (quickContent_ && watchedWidget
            && (watchedWidget == quickContent_.data()
                || quickContent_->isAncestorOf(watchedWidget))) {
            if (type == QEvent::Enter) {
                hideTimer_.stop();
            } else if (type == QEvent::Leave) {
                scheduleHide();
            }
            return QObject::eventFilter(watched, event);
        }

        if (type != QEvent::Enter && type != QEvent::Leave
            && type != QEvent::MouseButtonRelease && type != QEvent::Destroy) {
            return QObject::eventFilter(watched, event);
        }

        auto* button = qobject_cast<QPushButton*>(watched);
        QObject* owner = button
            ? button->property("matterleastPostOwner").value<QObject*>()
            : nullptr;
        auto* post = qobject_cast<PostWidget*>(owner);
        if (!post || button != post->reactionAffordance_) {
            return QObject::eventFilter(watched, event);
        }

        if (type == QEvent::Enter) {
            hideTimer_.stop();
            showFor(*post, *button);
        } else if (type == QEvent::Leave) {
            scheduleHide();
        } else if (type == QEvent::MouseButtonRelease) {
            // Do not mutate toolbar geometry between QAbstractButton's press and
            // release handling. Moving the reaction button during that interval
            // can suppress clicked(), so the complete emoji chooser never opens.
            // Collapse only after the release has reached the button.
            QTimer::singleShot(0, this, [this] { clearQuickBar(); });
        } else if (type == QEvent::Destroy) {
            clearQuickBar();
        }

        return QObject::eventFilter(watched, event);
    }

private:
    ReactionQuickBarController()
    {
        hideTimer_.setSingleShot(true);
        hideTimer_.setInterval(180);
        connect(&hideTimer_, &QTimer::timeout, this, [this] {
            animateSlot(false);
        });
    }

    void scheduleHide()
    {
        if (activeSlot_ && quickContent_) {
            hideTimer_.start();
        }
    }

    void updateToolbarGeometry()
    {
        if (!activePost_ || !activePost_->hoverActions_) {
            return;
        }
        if (activeSlot_) {
            activeSlot_->updateGeometry();
        }
        activePost_->hoverActions_->adjustSize();
        activePost_->positionHoverActions();
    }

    void clearQuickBar()
    {
        hideTimer_.stop();
        ++animationGeneration_;

        if (animation_) {
            animation_->stop();
            animation_->deleteLater();
            animation_.clear();
        }

        QPointer<PostWidget> post = activePost_;
        QPointer<QWidget> slot = activeSlot_;
        QWidget* content = quickContent_.data();

        if (slot) {
            slot->setMaximumWidth(0);
        }
        if (content) {
            content->hide();
            content->deleteLater();
        }

        // Commit the collapsed layout before forgetting the owning post. Without
        // this reflow the floating frame keeps its old expanded size and leaves
        // the empty slab visible after the quick-reaction content is removed.
        if (slot) {
            slot->updateGeometry();
        }
        if (post && post->hoverActions_) {
            post->hoverActions_->adjustSize();
            post->positionHoverActions();
        }

        quickContent_.clear();
        activeSlot_.clear();
        activePost_.clear();
        activeHeart_.clear();
        targetWidth_ = 0;
    }

    void animateSlot(bool expand)
    {
        if (!activeSlot_ || !activePost_ || !quickContent_) {
            return;
        }

        hideTimer_.stop();
        ++animationGeneration_;
        const quint64 generation = animationGeneration_;

        if (animation_) {
            animation_->stop();
            animation_->deleteLater();
            animation_.clear();
        }

        const int currentWidth = activeSlot_->maximumWidth();
        const int endWidth = expand ? targetWidth_ : 0;
        if (currentWidth == endWidth) {
            if (!expand) {
                clearQuickBar();
            }
            return;
        }

        auto* animation = new QPropertyAnimation(
            activeSlot_.data(), "maximumWidth", activeSlot_.data());
        animation_ = animation;
        animation->setDuration(110);
        animation->setStartValue(currentWidth);
        animation->setEndValue(endWidth);
        animation->setEasingCurve(
            expand ? QEasingCurve::OutCubic : QEasingCurve::InCubic);

        connect(animation, &QPropertyAnimation::valueChanged, this,
                [this, generation](const QVariant&) {
            if (generation == animationGeneration_) {
                updateToolbarGeometry();
            }
        });

        connect(animation, &QPropertyAnimation::finished, this,
                [this, generation, expand] {
            if (generation != animationGeneration_) {
                return;
            }
            animation_.clear();
            if (!expand) {
                clearQuickBar();
                return;
            }
            updateToolbarGeometry();
        });

        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void showFor(PostWidget& post, QPushButton& heart)
    {
        if (post.post.isDeleted || !post.hoverActions_
            || !post.reactionQuickBarSlot_) {
            clearQuickBar();
            return;
        }

        const QStringList quickNames =
            RankedEmojiPresentation::renderableNames(
                post.getBackend().emojiRegistry(),
                ReactionUsageTracker::instance().topNames(10)).mid(0, 8);
        if (quickNames.isEmpty()) {
            clearQuickBar();
            return;
        }

        if (activePost_ == &post && activeHeart_ == &heart
            && quickContent_) {
            animateSlot(true);
            return;
        }

        clearQuickBar();

        auto* slotLayout =
            qobject_cast<QHBoxLayout*>(post.reactionQuickBarSlot_->layout());
        if (!slotLayout) {
            return;
        }

        activePost_ = &post;
        activeHeart_ = &heart;
        activeSlot_ = post.reactionQuickBarSlot_;

        auto* content = new QWidget(activeSlot_.data());
        quickContent_ = content;
        content->setObjectName(QStringLiteral("reactionQuickBarInline"));
        content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

        auto* layout = new QHBoxLayout(content);
        // The 1 px right margin is the explicit separator from the reaction
        // action. Because it lives inside the animated zero-width slot, it does
        // not exist at all while the slot is collapsed.
        layout->setContentsMargins(0, 0, 1, 0);
        layout->setSpacing(1);

        QPointer<PostWidget> postGuard(&post);
        for (const QString& name : quickNames) {
            auto* reaction = new QPushButton(content);
            reaction->setFlat(true);
            reaction->setFixedSize(28, 28);
            reaction->setCursor(Qt::PointingHandCursor);
            if (!RankedEmojiPresentation::configureButton(
                    post.getBackend().emojiRegistry(), *reaction, name)) {
                delete reaction;
                continue;
            }

            reaction->setAccessibleName(tr("React with :%1:").arg(name));
            layout->addWidget(reaction);

            connect(reaction, &QPushButton::clicked, this,
                    [this, postGuard, name] {
                if (postGuard && !postGuard->post.isDeleted) {
                    postGuard->getBackend().addPostReaction(
                        postGuard->post.id, name);
                }
                animateSlot(false);
            });
        }

        if (layout->count() == 0) {
            clearQuickBar();
            return;
        }

        slotLayout->addWidget(content);

        targetWidth_ = layout->sizeHint().width();
        activeSlot_->setMinimumWidth(0);
        activeSlot_->setMaximumWidth(0);
        content->show();

        updateToolbarGeometry();
        animateSlot(true);
    }

    QTimer hideTimer_;
    QPointer<QWidget> activeSlot_;
    QPointer<QWidget> quickContent_;
    QPointer<PostWidget> activePost_;
    QPointer<QPushButton> activeHeart_;
    QPointer<QPropertyAnimation> animation_;
    int targetWidth_ = 0;
    quint64 animationGeneration_ = 0;
};

namespace {

void installReactionQuickBarController()
{
    QCoreApplication* application = QCoreApplication::instance();
    if (!application) {
        return;
    }

    application->installEventFilter(&ReactionQuickBarController::instance());
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installReactionQuickBarController)

} // namespace Mattermost
