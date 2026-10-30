#include "SpellChecker.h"
#if defined(Q_OS_LINUX) && defined(MATTERLEAST_HAVE_HUNSPELL)
#include <memory>
#include <vector>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStandardPaths>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#else
#include <QTextCodec>
#endif
#if __has_include(<hunspell/hunspell.hxx>)
#include <hunspell/hunspell.hxx>
#else
#include <hunspell.hxx>
#endif
namespace Mattermost {
namespace {
struct Dictionary {
    std::unique_ptr<Hunspell> hunspell;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    std::unique_ptr<QStringEncoder> encoder;
    std::unique_ptr<QStringDecoder> decoder;
#else
    QTextCodec* codec = nullptr;
#endif
    QByteArray encode(const QString& word) const
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return encoder->encode(word);
#else
        return codec->fromUnicode(word);
#endif
    }
    QString decode(const std::string& value) const
    {
        const QByteArray bytes(value.data(), static_cast<int>(value.size()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return decoder->decode(bytes);
#else
        return codec->toUnicode(bytes);
#endif
    }
};
class HunspellBackend final : public SpellBackend {
public:
    HunspellBackend()
    {
        QStringList paths {
            QStringLiteral("/usr/share/hunspell"),
            QStringLiteral("/usr/share/myspell"),
            QStringLiteral("/usr/share/myspell/dicts"),
            QStringLiteral("/usr/local/share/hunspell"),
            QStringLiteral("/usr/local/share/myspell")
        };
        paths += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                           QStringLiteral("hunspell"),
                                           QStandardPaths::LocateDirectory);
        paths += QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                           QStringLiteral("myspell"),
                                           QStandardPaths::LocateDirectory);
        const QString env = QString::fromLocal8Bit(qgetenv("MYSPELL_DICT_DIR"));
        if (!env.isEmpty())
            paths.prepend(env);
        for (const QString& path : paths) {
            const QDir directory(path);
            for (const QFileInfo& dic : directory.entryInfoList(
                     { QStringLiteral("*.dic") }, QDir::Files | QDir::Readable)) {
                const QFileInfo aff(directory.filePath(dic.completeBaseName()
                                                    + QStringLiteral(".aff")));
                if (aff.isReadable() && !_paths.contains(dic.completeBaseName()))
                    _paths.insert(dic.completeBaseName(),
                                  { aff.absoluteFilePath(), dic.absoluteFilePath() });
            }
        }
    }
    QStringList languages() const override { return _paths.keys(); }
    void configure(const QStringList& names) override
    {
        _loaded.clear();
        for (QString name : names) {
            name.replace(QLatin1Char('-'), QLatin1Char('_'));
            if (!_paths.contains(name)) {
                for (auto it = _paths.cbegin(); it != _paths.cend(); ++it) {
                    if (it.key().startsWith(name.left(2) + QLatin1Char('_'))) {
                        name = it.key();
                        break;
                    }
                }
            }
            if (!_paths.contains(name))
                continue;
            const auto pair = _paths.value(name);
            auto entry = std::make_unique<Dictionary>();
            entry->hunspell = std::make_unique<Hunspell>(
                QFile::encodeName(pair.first).constData(),
                QFile::encodeName(pair.second).constData());
            QByteArray encoding(entry->hunspell->get_dic_encoding());
            if (encoding.startsWith("microsoft-cp125"))
                encoding.replace(0, 12, "Windows-");
            if (encoding == "TIS620-2533")
                encoding = "TIS620";
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            entry->encoder = std::make_unique<QStringEncoder>(encoding.constData());
            entry->decoder = std::make_unique<QStringDecoder>(encoding.constData());
            if (!entry->encoder->isValid() || !entry->decoder->isValid())
                continue;
#else
            entry->codec = QTextCodec::codecForName(encoding);
            if (!entry->codec)
                continue;
#endif
            _loaded.push_back(std::move(entry));
        }
    }
    bool available() const override { return !_loaded.empty(); }
    bool correct(const QString& word) const override
    {
        for (const auto& entry : _loaded) {
            const QByteArray value = entry->encode(word);
            if (entry->hunspell->spell(std::string(value.constData(), size_t(value.size()))))
                return true;
        }
        return false;
    }
    QStringList suggestions(const QString& word) const override
    {
        QStringList result;
        for (const auto& entry : _loaded) {
            const QByteArray value = entry->encode(word);
            const auto guesses = entry->hunspell->suggest(
                std::string(value.constData(), size_t(value.size())));
            for (const auto& guess : guesses) {
                const QString text = entry->decode(guess);
                if (!result.contains(text))
                    result.append(text);
                if (result.size() >= 8)
                    return result;
            }
        }
        return result;
    }
private:
    QMap<QString, QPair<QString, QString>> _paths;
    std::vector<std::unique_ptr<Dictionary>> _loaded;
};
} // namespace
std::unique_ptr<SpellBackend> createPlatformSpellBackend()
{
    return std::make_unique<HunspellBackend>();
}
} // namespace Mattermost
#endif
