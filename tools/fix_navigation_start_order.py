from pathlib import Path
p = Path('sources/navigation/AppNavigationService.cpp')
text = p.read_text()
old = '''quint64 AppNavigationService::beginNavigation()\n{\n    const quint64 generation = navigationRequests.begin();\n    emit navigationStarted();\n    return generation;\n}\n'''
new = '''quint64 AppNavigationService::beginNavigation()\n{\n    // The service may have been instantiated before MainWindow existed. Make\n    // sure the synchronous invalidation signal has a receiver before publishing\n    // the new generation.\n    ensureMainWindowConnection();\n    const quint64 generation = navigationRequests.begin();\n    emit navigationStarted();\n    return generation;\n}\n'''
if old not in text:
    raise SystemExit('beginNavigation pattern not found')
p.write_text(text.replace(old, new, 1))
