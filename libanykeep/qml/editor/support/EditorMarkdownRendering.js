.pragma library

function formattedLinkLabelRuns(label) {
    const runs = []
    let buffer = ""
    let bold = false
    let italic = false
    let strike = false
    let codeDelimiter = ""
    let foundFormatting = false

    function appendRun() {
        if (buffer.length === 0)
            return
        runs.push({
            text: buffer,
            bold: bold,
            italic: italic,
            strike: strike,
            code: codeDelimiter.length > 0
        })
        buffer = ""
    }

    function backtickRunAt(position) {
        let end = position
        while (end < label.length && label.charAt(end) === "`")
            ++end
        return label.substring(position, end)
    }

    function isWordCharacter(character) {
        if (!character)
            return false
        return /[0-9A-Za-z]/.test(character)
                || character.toUpperCase() !== character.toLowerCase()
    }

    function underscoreIsIntraword(position, length) {
        const previous = position > 0 ? label.charAt(position - 1) : ""
        const next = position + length < label.length
                   ? label.charAt(position + length) : ""
        return isWordCharacter(previous) && isWordCharacter(next)
    }

    for (let index = 0; index < label.length;) {
        const character = label.charAt(index)
        if (character === "\\" && index + 1 < label.length) {
            buffer += label.substring(index, index + 2)
            index += 2
            continue
        }

        if (character === "`") {
            const delimiter = backtickRunAt(index)
            if (codeDelimiter.length === 0 || codeDelimiter === delimiter) {
                appendRun()
                codeDelimiter = codeDelimiter.length === 0 ? delimiter : ""
                foundFormatting = true
                index += delimiter.length
                continue
            }
        }

        if (codeDelimiter.length === 0) {
            const pair = label.substring(index, index + 2)
            if (pair === "**" || (pair === "__" && !underscoreIsIntraword(index, 2))) {
                appendRun()
                bold = !bold
                foundFormatting = true
                index += 2
                continue
            }
            if (pair === "~~") {
                appendRun()
                strike = !strike
                foundFormatting = true
                index += 2
                continue
            }
            if (character === "*"
                    || (character === "_" && !underscoreIsIntraword(index, 1))) {
                appendRun()
                italic = !italic
                foundFormatting = true
                ++index
                continue
            }
        }

        buffer += character
        ++index
    }

    appendRun()
    if (!foundFormatting || bold || italic || strike || codeDelimiter.length > 0)
        return null
    return runs
}

function codeSpanForRendering(text) {
    let maximumRun = 0
    let currentRun = 0
    for (let index = 0; index < text.length; ++index) {
        if (text.charAt(index) === "`") {
            ++currentRun
            maximumRun = Math.max(maximumRun, currentRun)
        } else {
            currentRun = 0
        }
    }
    let delimiter = ""
    for (let index = 0; index <= maximumRun; ++index)
        delimiter += "`"
    return delimiter + text + delimiter
}

function renderFormattedLinkRun(run) {
    let value = run.code ? codeSpanForRendering(run.text) : run.text
    if (run.italic)
        value = "*" + value + "*"
    if (run.bold)
        value = "**" + value + "**"
    if (run.strike)
        value = "~~" + value + "~~"
    return value
}

function markdownForRendering(source) {
    let rendered = underlineMarkupForRendering(source || "")
    if (!rendered || (rendered.indexOf("*") < 0 && rendered.indexOf("_") < 0
                      && rendered.indexOf("~") < 0 && rendered.indexOf("`") < 0))
        return rendered

    // QTextDocument can lose nested or intraword formatting inside a
    // link label. Give every uniform style run its own adjacent link.
    // The C++ serializer joins the runs back into one Markdown link.
    const linkPattern = /\[((?:\\.|[^\]\\\n])*)\]\(([^)\n]+)\)/g
    return rendered.replace(linkPattern, function(fullMatch, label, destination, offset, wholeText) {
        if (offset > 0 && wholeText.charAt(offset - 1) === "!")
            return fullMatch

        const runs = formattedLinkLabelRuns(label)
        if (runs === null)
            return fullMatch

        let result = ""
        for (const run of runs) {
            if (run.text.length === 0)
                continue
            result += "[" + renderFormattedLinkRun(run) + "](" + destination + ")"
        }
        return result.length > 0 ? result : fullMatch
    })
}

function underlineMarkupForRendering(source) {
    const underlinePattern = /^<(ins|u)(?:\s[^>]*)?>([\s\S]*?)<\/\1\s*>/i
    let result = ""
    let codeDelimiter = ""
    for (let index = 0; index < source.length;) {
        const character = source.charAt(index)
        if (character === "\\" && index + 1 < source.length) {
            result += source.substring(index, index + 2)
            index += 2
            continue
        }
        if (character === "`") {
            let end = index
            while (end < source.length && source.charAt(end) === "`")
                ++end
            const delimiter = source.substring(index, end)
            if (codeDelimiter.length === 0 || codeDelimiter === delimiter)
                codeDelimiter = codeDelimiter.length === 0 ? delimiter : ""
            result += delimiter
            index = end
            continue
        }
        if (codeDelimiter.length === 0 && character === "<") {
            const match = source.substring(index).match(underlinePattern)
            if (match) {
                result += "ANYKEEPINSOPEN7F3A" + match[2] + "ANYKEEPINSCLOSE7F3A"
                index += match[0].length
                continue
            }
        }
        result += character
        ++index
    }
    return result
}

function markdownTableCellForRendering(source) {
    // The model stores a table-cell hard break as a newline and writes it
    // as <br>. Recreate Qt's in-document line separator directly so the
    // editable value remains one QTextDocument block.
    return markdownForRendering(source).replace(/\n/g, "\u2028")
}
