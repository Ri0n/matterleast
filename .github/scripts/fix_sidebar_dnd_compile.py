from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def p(path): return ROOT / path
def read(path): return p(path).read_text()
def write(path, text): p(path).write_text(text)
def rep(path, old, new):
    text = read(path)
    if text.count(old) != 1:
        raise RuntimeError(f"anchor count {text.count(old)} in {path}: {old[:100]!r}")
    write(path, text.replace(old, new, 1))

# storeCategories' outer lambda must carry storeResponse into the nested status
# resolver callback.
rep("sources/backend/SidebarService.cpp",
    "        [this, teamId = std::move(teamId), statePtr, directUserIds, callback] {",
    "        [this, teamId = std::move(teamId), statePtr, directUserIds, callback, storeResponse] {")

# Retrieval must distinguish a failed GET from an authoritative empty tree.
rep("sources/backend/SidebarService.h",
    "    void retrieveCategories(BackendTeam& team,\n"
    "                            std::function<void(const SidebarTeamState&)> callback = {},\n"
    "                            bool storeResponse = true);",
    "    void retrieveCategories(BackendTeam& team,\n"
    "                            std::function<void(const SidebarTeamState&)> callback = {},\n"
    "                            std::function<void()> errorCallback = {},\n"
    "                            bool storeResponse = true);")

rep("sources/backend/SidebarService.cpp",
    "void SidebarService::retrieveCategories(BackendTeam& team,\n"
    "                                        std::function<void(const SidebarTeamState&)> callback,\n"
    "                                        bool storeResponse)\n",
    "void SidebarService::retrieveCategories(BackendTeam& team,\n"
    "                                        std::function<void(const SidebarTeamState&)> callback,\n"
    "                                        std::function<void()> errorCallback,\n"
    "                                        bool storeResponse)\n")
rep("sources/backend/SidebarService.cpp",
    "    retrieveChannelMemberships([this, teamId, callback, storeResponse] {\n"
    "        retrieveChannelPreferences([this, teamId, callback, storeResponse] {",
    "    retrieveChannelMemberships([this, teamId, callback, errorCallback, storeResponse] {\n"
    "        retrieveChannelPreferences([this, teamId, callback, errorCallback, storeResponse] {")
rep("sources/backend/SidebarService.cpp",
    "            httpConnector.get(request, HttpResponseCallback(\n"
    "                [this, teamId, callback, storeResponse](const QJsonDocument& doc) {\n"
    "                    SidebarTeamState state;",
    "            httpConnector.get(request, HttpResponseCallback(\n"
    "                [this, teamId, callback, errorCallback, storeResponse](\n"
    "                    QVariant status, const QJsonDocument& doc) {\n"
    "                    if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {\n"
    "                        if (errorCallback) {\n"
    "                            errorCallback();\n"
    "                        }\n"
    "                        return;\n"
    "                    }\n"
    "                    SidebarTeamState state;")

# Verification passes an explicit no-op error handler and requests a snapshot
# without storing it in SidebarService until the mutation generation matches.
rep("sources/channel-tree/ChannelTreeReconcile.cpp",
    "        },\n"
    "        false);\n}\n\nvoid ChannelTree::destroySidebarRow",
    "        },\n"
    "        {},\n"
    "        false);\n}\n\nvoid ChannelTree::destroySidebarRow")

# The create-group path is unrelated to optimistic DnD and must keep its normal
# refresh callback. The previous mechanical replacement accidentally referenced
# a non-existent mutation variable there.
rep("sources/channel-tree/ChannelTree.cpp",
    "            sidebar.updateCategories(teamId, updates,\n"
    "                [guard, teamId](const SidebarTeamState&) {\n"
    "                    if (guard) {\n"
    "                        guard->verifySidebarTeam(teamId, mutation);\n"
    "                    }\n"
    "                });",
    "            sidebar.updateCategories(teamId, updates,\n"
    "                [guard, teamId](const SidebarTeamState&) {\n"
    "                    if (guard) {\n"
    "                        guard->refreshSidebarTeam(teamId);\n"
    "                    }\n"
    "                });")

# Category DnD success uses the same generation-checked verification as channel
# DnD rather than an ordinary storing refresh.
rep("sources/channel-tree/ChannelTree.cpp",
    "        [guard, teamId, mutation] {\n"
    "            if (guard && guard->sidebarMutationGeneration.value(teamId) == mutation) {\n"
    "                guard->refreshSidebarTeam(teamId);\n"
    "            }\n"
    "        },\n"
    "        rollback,\n"
    "        false);",
    "        [guard, teamId, mutation] {\n"
    "            if (guard && guard->sidebarMutationGeneration.value(teamId) == mutation) {\n"
    "                guard->verifySidebarTeam(teamId, mutation);\n"
    "            }\n"
    "        },\n"
    "        rollback,\n"
    "        false);")

print("verification fixes applied")
