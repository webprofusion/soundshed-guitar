#pragma once

// JsonSkimmer.h — reading a few values out of a large JSON document.
//
// A .nam model is one JSON document whose weights array is nearly all of it, and the
// library reads hundreds of them per folder scan for a handful of top-level fields.
// JsonSkimmer streams the document through a fixed buffer and hands the caller each
// member of an object in turn; whatever the caller does not ask for is stepped over in
// place, never parsed into a DOM or copied.

#include "util/Utf8.h"

#include <cstddef>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx::util
{
/// The value of a JSON string literal (quotes included) with its escapes decoded. Text
/// read raw off disk is not trusted to be UTF-8: a file written in a
/// legacy code page has its stray bytes replaced with U+FFFD, and a literal the parser
/// still rejects (a bad escape, a raw control character) comes back as written. Either
/// way the result is valid UTF-8, which is what lets it go into a JSON message or the
/// library without dump() throwing.
[[nodiscard]] inline std::string DecodeJsonStringLiteral(std::string_view literal)
{
    const std::string utf8 = ReplaceInvalidUtf8(literal);

    try
    {
        return nlohmann::json::parse(utf8).get<std::string>();
    }
    catch (const nlohmann::json::exception&)
    {
        return utf8.substr(1, utf8.size() - 2);
    }
}

/// Reads JSON from a stream through a fixed buffer and visits only the values the caller
/// asks for. Everything else is skipped in place: the weights array, most of a .nam
/// file, is stepped over a byte at a time and never becomes a DOM or a copy.
class JsonSkimmer
{
  public:
    explicit JsonSkimmer(std::istream& in) : mIn(in), mBuf(kBufSize)
    {
    }

    /// Visits each member of the object at the cursor. `onMember(key)` is called with the
    /// cursor on that member's value and must consume it. False on malformed input, or
    /// when `onMember` returns false.
    template <typename OnMember> bool ForEachMember(OnMember&& onMember)
    {
        if (!Consume('{'))
        {
            return false;
        }

        if (Consume('}'))
        {
            return true;
        }

        std::string rawKey;

        for (;;)
        {
            SkipWhitespace();
            rawKey.clear();

            if (!ReadStringLiteral(rawKey) || !Consume(':') || !onMember(KeyText(rawKey)))
            {
                return false;
            }

            if (Consume(','))
            {
                continue;
            }

            return Consume('}');
        }
    }

    /// The same for each element of the array at the cursor.
    template <typename OnElement> bool ForEachElement(OnElement&& onElement)
    {
        if (!Consume('['))
        {
            return false;
        }

        if (Consume(']'))
        {
            return true;
        }

        for (;;)
        {
            if (!onElement())
            {
                return false;
            }

            if (Consume(','))
            {
                continue;
            }

            return Consume(']');
        }
    }

    /// The first non-whitespace byte at the cursor, or -1 at the end.
    int PeekValue()
    {
        SkipWhitespace();
        return Peek();
    }

    /// A string value, decoded. Any other value is skipped and leaves `out` empty.
    bool ReadString(std::string& out)
    {
        if (PeekValue() != '"')
        {
            return SkipValue();
        }

        std::string raw;

        if (!ReadStringLiteral(raw))
        {
            return false;
        }

        out = DecodeJsonStringLiteral(raw);
        return true;
    }

    /// A number value, as written. Any other value (null most often) is skipped and
    /// leaves `out` empty. `out` keeps its capacity, so one string can be reused across a
    /// whole array of numbers without allocating per element.
    bool ReadNumber(std::string& out)
    {
        out.clear();
        const int first = PeekValue();

        if (first != '-' && (first < '0' || first > '9'))
        {
            return SkipValue();
        }

        for (int c = Peek(); c >= 0 && !IsDelimiter(c); c = Peek())
        {
            out.push_back(static_cast<char>(c));
            ++mPos;
        }

        return true;
    }

    /// Steps over one value of any kind, nested ones included.
    bool SkipValue()
    {
        const int first = PeekValue();

        if (first < 0)
        {
            return false;
        }

        if (first == '"')
        {
            return SkipStringLiteral();
        }

        if (first != '{' && first != '[')
        {
            // A number or a bare literal (true, false, null, and the NaN a Python
            // trainer can write): everything up to the next delimiter.
            for (int c = Peek(); c >= 0 && !IsDelimiter(c); c = Peek())
            {
                ++mPos;
            }

            return true;
        }

        int depth = 0;

        for (;;)
        {
            if (mPos == mEnd && !Refill())
            {
                return false;
            }

            while (mPos < mEnd)
            {
                const char c = mBuf[mPos];

                if (c != '"' && c != '{' && c != '}' && c != '[' && c != ']')
                {
                    ++mPos;
                    continue;
                }

                if (c == '"')
                {
                    if (!SkipStringLiteral())
                    {
                        return false;
                    }

                    continue;
                }

                ++mPos;

                if (c == '{' || c == '[')
                {
                    ++depth;
                }
                else if (--depth == 0)
                {
                    return true;
                }
            }
        }
    }

  private:
    static constexpr std::size_t kBufSize = 64 * 1024;

    static bool IsDelimiter(int c)
    {
        return c == ',' || c == '}' || c == ']' || c == ':' || c == '"' || c == '{' || c == '[' || c == ' ' ||
               c == '\t' || c == '\n' || c == '\r';
    }

    /// A key as text. Keys carry no escapes in practice, so only one that does is decoded.
    static std::string KeyText(const std::string& raw)
    {
        if (raw.find('\\') == std::string::npos)
        {
            return raw.substr(1, raw.size() - 2);
        }

        return DecodeJsonStringLiteral(raw);
    }

    bool Refill()
    {
        if (!mIn)
        {
            return false;
        }

        mIn.read(mBuf.data(), static_cast<std::streamsize>(mBuf.size()));
        mPos = 0;
        mEnd = static_cast<std::size_t>(mIn.gcount());
        return mEnd > 0;
    }

    int Peek()
    {
        if (mPos == mEnd && !Refill())
        {
            return -1;
        }

        return static_cast<unsigned char>(mBuf[mPos]);
    }

    void SkipWhitespace()
    {
        for (int c = Peek(); c == ' ' || c == '\t' || c == '\n' || c == '\r'; c = Peek())
        {
            ++mPos;
        }
    }

    bool Consume(char expected)
    {
        if (PeekValue() != static_cast<unsigned char>(expected))
        {
            return false;
        }

        ++mPos;
        return true;
    }

    /// Walks the string literal at the cursor, appending its bytes, quotes included, to
    /// `out` when one is given. A backslash escapes the byte after it, so \" does not end it.
    bool WalkStringLiteral(std::string* out)
    {
        if (Peek() != '"')
        {
            return false;
        }

        if (out)
        {
            out->push_back('"');
        }

        ++mPos;
        bool escaped = false;

        for (;;)
        {
            if (mPos == mEnd && !Refill())
            {
                return false;
            }

            const std::size_t start = mPos;

            while (mPos < mEnd)
            {
                const char c = mBuf[mPos++];

                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    if (out)
                    {
                        out->append(mBuf.data() + start, mPos - start);
                    }

                    return true;
                }
            }

            if (out)
            {
                out->append(mBuf.data() + start, mPos - start);
            }
        }
    }

    bool ReadStringLiteral(std::string& out)
    {
        return WalkStringLiteral(&out);
    }

    bool SkipStringLiteral()
    {
        return WalkStringLiteral(nullptr);
    }

    std::istream& mIn;
    std::vector<char> mBuf;
    std::size_t mPos = 0;
    std::size_t mEnd = 0;
};
} // namespace guitarfx::util
