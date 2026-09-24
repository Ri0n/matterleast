#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace Mattermost {

class BackendPost;

/**
 * Logical post sequence consumed by ChatLogWidget.
 *
 * A source owns post identity/range retrieval only. It never manipulates the
 * view, scrollbar, widget geometry or materialization.
 */
class AbstractPostSource : public QObject
{
    Q_OBJECT
public:
    enum class RequestReason {
        Initial,
        Scroll,
        Seek,
        EnsureVisible,
    };
    Q_ENUM(RequestReason)

    explicit AbstractPostSource(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~AbstractPostSource() override = default;

    virtual int itemCount() const = 0;
    virtual bool isAvailable(int index) const = 0;
    virtual BackendPost* postAt(int index) const = 0;

    /**
     * Stable semantic identity for a logical row, even when its body is not
     * currently resident. Empty means the slot has not been resolved yet.
     */
    virtual QString postIdAt(int index) const = 0;

    virtual int indexOfPost(const QString& postId) const = 0;

    /**
     * Presentation decorators override this to expose the source they wrap.
     * Authoritative server sources leave it null.
     *
     * Semantic operations that must reach transport/topology ownership (for
     * example permalink context adoption) must traverse this chain instead of
     * guessing concrete decorator types.
     */
    virtual AbstractPostSource* wrappedSource() const { return nullptr; }

    /**
     * Return the innermost source in the decorator chain. The method is cycle
     * safe so a malformed decorator cannot hang navigation.
     */
    AbstractPostSource* authoritativeSource()
    {
        AbstractPostSource* current = this;
        QSet<AbstractPostSource*> visited;
        while (current && !visited.contains(current)) {
            visited.insert(current);
            AbstractPostSource* wrapped = current->wrappedSource();
            if (!wrapped || visited.contains(wrapped)) {
                break;
            }
            current = wrapped;
        }
        return current;
    }

    const AbstractPostSource* authoritativeSource() const
    {
        return const_cast<AbstractPostSource*>(this)->authoritativeSource();
    }

    /**
     * Return a logical index for an already cached semantic target. Sources may
     * temporarily place a target into an estimated empty slot when the server
     * has not yet supplied an authoritative page boundary. Later page loads are
     * allowed to replace that estimate by identity. An estimated identity should
     * remain unavailable until the source is willing to let the view materialize
     * a concrete widget at that logical position.
     */
    virtual int ensurePostIndex(const QString& postId)
    {
        return indexOfPost(postId);
    }

    /**
     * Whether this view-facing row belongs to authoritative server history.
     * Presentation augmentations override this for local-only rows.
     */
    virtual bool isAuthoritativeRow(int index) const
    {
        return index >= 0 && index < itemCount();
    }

    /** Number of authoritative rows in the view-facing sequence. */
    virtual int authoritativeItemCount() const { return itemCount(); }

    /**
     * Whether the semantic post already has a server-confirmed logical
     * position. Decorators delegate this to the wrapped source.
     */
    virtual bool isPostPositionAuthoritative(const QString& postId) const
    {
        return indexOfPost(postId) >= 0;
    }

    virtual void requestRange(int first,
                              int last,
                              RequestReason reason,
                              quint64 generation) = 0;

    /**
     * Compatibility path for a sequence whose oldest boundary is not yet known.
     * Exact-count sources leave this disabled and use requestRange() normally.
     */
    virtual bool canRequestBeforeFirst() const { return false; }
    virtual void requestBeforeFirst(RequestReason reason, quint64 generation)
    {
        Q_UNUSED(reason)
        Q_UNUSED(generation)
    }

protected:
    /**
     * Explicitly publish a genuine semantic identity-to-index remap.
     * Do not use this for body/reaction/deletion/presentation updates.
     */
    void mappingChanged(int first, int last) { emit layoutChanged(first, last); }

signals:
    void itemCountChanged(int count);

    /** Existing logical items shifted right because real items were prepended. */
    void itemsInserted(int first, int count);

    /** Existing logical items were removed and later identities shifted left. */
    void itemsRemoved(int first, int count);

    void rangeAvailable(int first, int last);

    /** Initial approximate seek resolved to a concrete row in this generation. */
    void seekTargetResolved(int index, quint64 generation);

    /** Body residency changed and therefore may change effective row availability. */
    void bodyAvailabilityChanged(int first, int last, bool available);

    /**
     * Identity-to-index mapping changed in this logical span. Existing widgets
     * representing surviving identities must be moved, never recreated.
     */
    void layoutChanged(int first, int last);

    /** Every requestRange() call must eventually emit this exact requested range. */
    void rangeRequestFinished(int first, int last);
};

} // namespace Mattermost
