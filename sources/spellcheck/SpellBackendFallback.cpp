#include "SpellChecker.h"
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && \
    !(defined(Q_OS_LINUX) && defined(MATTERLEAST_HAVE_HUNSPELL))
namespace Mattermost {
std::unique_ptr<SpellBackend> createPlatformSpellBackend()
{
    return {};
}
} // namespace Mattermost
#endif
