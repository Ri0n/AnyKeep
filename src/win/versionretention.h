#pragma once

#include <algorithm>
#include <cwctype>
#include <string>

namespace AnyKeepWin {

// The cleanup policy is intentionally platform-neutral so that destructive
// decisions can be tested without a Windows installation.
struct VersionRetention {
    std::wstring current;
    std::wstring previous;
    std::wstring msiInstalled;
    std::wstring prepared;
    bool         pointersValid = false;
    bool         preparedValid = false;
};

struct VersionCandidate {
    std::wstring name;
    bool         expired = false;
    bool         inUse = false;
    bool         reparsePoint = false;
};

inline bool safeVersionName(const std::wstring &name)
{
    if (name.empty() || name.size() > 80 || name == L"." || name == L"..")
        return false;
    return std::all_of(name.begin(), name.end(), [](wchar_t ch) {
        const bool asciiAlphanumeric
            = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        return asciiAlphanumeric || ch == L'.' || ch == L'-' || ch == L'_' || ch == L'+';
    });
}

inline bool sameVersion(const std::wstring &left, const std::wstring &right)
{
    return !left.empty() && !right.empty()
        && left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
               return std::towlower(a) == std::towlower(b);
           });
}

inline bool canRemoveVersion(const VersionCandidate &candidate, const VersionRetention &retention)
{
    // An invalid pointer must never be interpreted as a version to discard.
    // Wait for a successful launcher repair or a completed update instead.
    if (!retention.pointersValid || !retention.preparedValid || !safeVersionName(candidate.name)
        || !candidate.expired || candidate.inUse || candidate.reparsePoint)
        return false;
    return !sameVersion(candidate.name, retention.current)
        && !sameVersion(candidate.name, retention.previous)
        && !sameVersion(candidate.name, retention.msiInstalled)
        && !sameVersion(candidate.name, retention.prepared);
}

} // namespace AnyKeepWin
