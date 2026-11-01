/**
 * @file SpellBackendFallback.cpp
 * @brief Fallback for systems without a spell-checking provider.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

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
