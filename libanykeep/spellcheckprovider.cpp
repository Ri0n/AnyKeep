#include "spellcheckprovider.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextFragment>

#include "notehighlighter.h"

namespace AnyKeep {
namespace {
    class ProviderSpellCheckExtension final : public SpellCheckExtension {
    public:
        explicit ProviderSpellCheckExtension(std::shared_ptr<SpellCheckProvider> provider) :
            provider_(std::move(provider))
        {
            expression_
                = QRegularExpression(QStringLiteral("[[:alpha:]]{2,}"), QRegularExpression::UseUnicodePropertiesOption);
        }

        void reset() override { }

        QStringList suggestions(const QString &word) const override { return provider_->suggestions(word); }
        void        addToDictionary(const QString &word) override { provider_->addToDictionary(word); }

        void highlight(NoteHighlighter *highlighter, const QString &text) override
        {
            QTextCharFormat format;
            format.setProperty(SpellCheckFormatProperty, true);

            auto matches = expression_.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                if (!isInlineCode(highlighter->currentBlock(), match.capturedStart())
                    && !provider_->isCorrect(match.captured())) {
                    highlighter->addFormat(match.capturedStart(), match.capturedLength(), format);
                }
            }
        }

    private:
        static bool isInlineCode(const QTextBlock &block, int positionInBlock)
        {
            const int position = block.position() + positionInBlock;
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (fragment.isValid() && fragment.contains(position))
                    return fragment.charFormat().fontFixedPitch();
            }
            return false;
        }

        std::shared_ptr<SpellCheckProvider> provider_;
        QRegularExpression                  expression_;
    };
} // namespace

std::shared_ptr<SpellCheckExtension> makeSpellCheckExtension(const std::shared_ptr<SpellCheckProvider> &provider)
{
    if (!provider || !provider->isValid())
        return {};
    return std::make_shared<ProviderSpellCheckExtension>(provider);
}

} // namespace AnyKeep
