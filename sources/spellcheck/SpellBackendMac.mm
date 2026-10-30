#include "SpellChecker.h"
#ifdef Q_OS_MACOS
#import <AppKit/NSSpellChecker.h>
#import <Foundation/Foundation.h>
namespace Mattermost {
namespace {
NSString* nativeWord(const QString& word)
{
    return [NSString stringWithCharacters:reinterpret_cast<const unichar*>(word.utf16())
                                  length:static_cast<NSUInteger>(word.size())];
}
class MacSpellBackend final : public SpellBackend {
public:
    QStringList languages() const override { return {}; }
    void configure(const QStringList&) override {}
    bool available() const override { return true; }
    bool correct(const QString& word) const override
    {
        @autoreleasepool {
            NSString* text = nativeWord(word);
            const NSRange range = [[NSSpellChecker sharedSpellChecker]
                checkSpellingOfString:text startingAt:0 language:nil wrap:NO
                inSpellDocumentWithTag:0 wordCount:nullptr];
            return range.location == NSNotFound;
        }
    }
    QStringList suggestions(const QString& word) const override
    {
        @autoreleasepool {
            NSString* text = nativeWord(word);
            NSArray<NSString*>* guesses = [[NSSpellChecker sharedSpellChecker]
                guessesForWordRange:NSMakeRange(0, [text length])
                inString:text language:nil inSpellDocumentWithTag:0];
            QStringList result;
            for (NSString* guess in guesses) {
                result.append(QString::fromUtf8([guess UTF8String]));
                if (result.size() >= 8)
                    break;
            }
            return result;
        }
    }
};
} // namespace
std::unique_ptr<SpellBackend> createPlatformSpellBackend()
{
    return std::make_unique<MacSpellBackend>();
}
} // namespace Mattermost
#endif
