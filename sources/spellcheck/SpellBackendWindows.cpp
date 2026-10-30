#include "SpellChecker.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <spellcheck.h>
#include <wrl/client.h>
#include <QSet>
namespace Mattermost {
namespace {
using Microsoft::WRL::ComPtr;
class WindowsSpellBackend final : public SpellBackend {
public:
    WindowsSpellBackend()
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        _uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            return;
        CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(_factory.GetAddressOf()));
        if (!_factory)
            return;
        ComPtr<IEnumString> supported;
        if (FAILED(_factory->get_SupportedLanguages(supported.GetAddressOf())) || !supported)
            return;
        LPOLESTR item = nullptr;
        while (supported->Next(1, &item, nullptr) == S_OK) {
            _languages.append(QString::fromWCharArray(item));
            CoTaskMemFree(item);
            item = nullptr;
        }
    }
    ~WindowsSpellBackend() override
    {
        _checkers.clear();
        _factory.Reset();
        if (_uninitialize)
            CoUninitialize();
    }
    QStringList languages() const override { return _languages; }
    void configure(const QStringList& names) override
    {
        _checkers.clear();
        if (!_factory)
            return;
        QSet<QString> added;
        for (QString name : names) {
            name.replace(QLatin1Char('_'), QLatin1Char('-'));
            for (const QString& installed : _languages) {
                if (installed.compare(name, Qt::CaseInsensitive) != 0)
                    continue;
                if (added.contains(installed))
                    break;
                ComPtr<ISpellChecker> checker;
                const std::wstring locale = installed.toStdWString();
                if (SUCCEEDED(_factory->CreateSpellChecker(
                                  locale.c_str(), checker.GetAddressOf())) && checker) {
                    _checkers.append(checker);
                    added.insert(installed);
                }
                break;
            }
        }
    }
    bool available() const override { return !_checkers.isEmpty(); }
    bool correct(const QString& word) const override
    {
        const std::wstring value = word.toStdWString();
        for (const auto& checker : _checkers) {
            ComPtr<IEnumSpellingError> errors;
            if (FAILED(checker->Check(value.c_str(), errors.GetAddressOf())) || !errors)
                continue;
            ComPtr<ISpellingError> error;
            if (errors->Next(error.GetAddressOf()) != S_OK)
                return true;
        }
        return false;
    }
    QStringList suggestions(const QString& word) const override
    {
        QStringList result;
        const std::wstring value = word.toStdWString();
        for (const auto& checker : _checkers) {
            ComPtr<IEnumString> guesses;
            if (FAILED(checker->Suggest(value.c_str(), guesses.GetAddressOf())) || !guesses)
                continue;
            LPOLESTR item = nullptr;
            while (guesses->Next(1, &item, nullptr) == S_OK) {
                const QString suggestion = QString::fromWCharArray(item);
                CoTaskMemFree(item);
                item = nullptr;
                if (!result.contains(suggestion))
                    result.append(suggestion);
                if (result.size() >= 8)
                    return result;
            }
        }
        return result;
    }
private:
    bool _uninitialize = false;
    ComPtr<ISpellCheckerFactory> _factory;
    QStringList _languages;
    QList<ComPtr<ISpellChecker>> _checkers;
};
} // namespace
std::unique_ptr<SpellBackend> createPlatformSpellBackend()
{
    return std::make_unique<WindowsSpellBackend>();
}
} // namespace Mattermost
#endif
