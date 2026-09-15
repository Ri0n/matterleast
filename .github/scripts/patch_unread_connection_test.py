from pathlib import Path


def read(path):
    return Path(path).read_text()


def write(path, text):
    Path(path).write_text(text)


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing block in {path}: {old[:180]!r}")
    write(path, text.replace(old, new, 1))


# Qt::UniqueConnection is not defined for functors/lambdas. This connection is
# established from populateSidebars(), so keep the functor but do not ask Qt for
# unsupported uniqueness semantics; duplicate invocations are idempotent.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    "    connect(&sidebar, &SidebarService::channelActivityChanged, this,\n"
    "            [this, &sidebar](const QString& channelId) {\n"
    "        if (!backendForSidebar) {\n"
    "            return;\n"
    "        }\n"
    "        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);\n"
    "        if (channel) {\n"
    "            setChannelUnreadVisual(channelId, sidebar.isChannelUnread(*channel));\n"
    "        }\n"
    "    }, Qt::UniqueConnection);\n",
    "    connect(&sidebar, &SidebarService::channelActivityChanged, this,\n"
    "            [this, &sidebar](const QString& channelId) {\n"
    "        if (!backendForSidebar) {\n"
    "            return;\n"
    "        }\n"
    "        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);\n"
    "        if (channel) {\n"
    "            setChannelUnreadVisual(channelId, sidebar.isChannelUnread(*channel));\n"
    "        }\n"
    "    });\n")

# Rendering regression: the canonical UnreadRole must visibly affect a channel row.
replace_once(
    "tests/SidebarItemDelegateTest.cpp",
    "    static QImage renderItem(SidebarItem::Kind kind, int channelType, const QString& presence)\n",
    "    static QImage renderItem(SidebarItem::Kind kind, int channelType, const QString& presence,\n"
    "                             bool unread = false)\n")
replace_once(
    "tests/SidebarItemDelegateTest.cpp",
    "        item->setData(presence, SidebarItem::PresenceRole);\n",
    "        item->setData(presence, SidebarItem::PresenceRole);\n"
    "        item->setData(unread, SidebarItem::UnreadRole);\n")
replace_once(
    "tests/SidebarItemDelegateTest.cpp",
    "    void rendersPresenceForDirectMessage()\n",
    "    void unreadRoleMakesConversationVisuallyBold()\n"
    "    {\n"
    "        const auto read = renderItem(SidebarItem::Channel,\n"
    "                                     BackendChannel::publicChannel, QString(), false);\n"
    "        const auto unread = renderItem(SidebarItem::Channel,\n"
    "                                       BackendChannel::publicChannel, QString(), true);\n"
    "        QVERIFY(read != unread);\n"
    "    }\n\n"
    "    void rendersPresenceForDirectMessage()\n")
