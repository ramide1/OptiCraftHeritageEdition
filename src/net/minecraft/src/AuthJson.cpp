#include "AuthJson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
class Parser
{
public:
	explicit Parser(const std::string &document) : src(document) {}

	bool parseDocument(AuthJson::Value &out)
	{
		AuthJson::Value value;
		if (!parseValue(value))
			return false;
		skipSpace();
		if (!ok || pos != src.size())
			return false;
		out = std::move(value);
		return true;
	}

private:
	const std::string &src;
	std::size_t pos = 0;
	bool ok = true;

	void skipSpace()
	{
		while (pos < src.size())
		{
			const char c = src[pos];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				pos++;
			else
				break;
		}
	}

	bool fail()
	{
		ok = false;
		return false;
	}

	bool consume(char c)
	{
		skipSpace();
		if (pos >= src.size() || src[pos] != c)
			return fail();
		pos++;
		return true;
	}

	// Lookahead probe: like consume(), but a mismatch is NOT an error and
	// must not poison the sticky ok flag. All optional continuations (',',
	// '}', ']') go through here; consume() stays for committed input where
	// a mismatch really is malformed.
	bool tryConsume(char c)
	{
		skipSpace();
		if (pos >= src.size() || src[pos] != c)
			return false;
		pos++;
		return true;
	}

	bool literal(const char *text)
	{
		skipSpace();
		const std::size_t length = std::strlen(text);
		if (pos + length > src.size() || src.compare(pos, length, text) != 0)
			return fail();
		pos += length;
		return true;
	}

	static void encodeUtf8(std::string &out, unsigned code)
	{
		if (code >= 0xD800 && code <= 0xDFFF)
			code = 0xFFFD; // lone surrogates were never valid input anyway
		if (code < 0x80)
		{
			out.push_back(static_cast<char>(code));
		}
		else if (code < 0x800)
		{
			out.push_back(static_cast<char>(0xC0 | (code >> 6)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xE0 | (code >> 12)));
			out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
	}

	bool parseStringInto(std::string &out)
	{
		out.clear();
		for (;;)
		{
			if (pos >= src.size())
				return fail();
			const unsigned char c = static_cast<unsigned char>(src[pos++]);
			if (c == '"')
				return true;
			if (c != '\\')
			{
				out.push_back(static_cast<char>(c));
				continue;
			}
			if (pos >= src.size())
				return fail();
			const char escape = src[pos++];
			switch (escape)
			{
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u':
			{
				unsigned code = 0;
				for (int digit = 0; digit < 4; digit++)
				{
					if (pos >= src.size())
						return fail();
					const char half = src[pos++];
					code <<= 4;
					if (half >= '0' && half <= '9')
						code |= static_cast<unsigned>(half - '0');
					else if (half >= 'a' && half <= 'f')
						code |= static_cast<unsigned>(half - 'a' + 10);
					else if (half >= 'A' && half <= 'F')
						code |= static_cast<unsigned>(half - 'A' + 10);
					else
						return fail();
				}
				encodeUtf8(out, code);
				break;
			}
			default:
				return fail();
			}
		}
	}

	bool parseValue(AuthJson::Value &out)
	{
		skipSpace();
		if (!ok || pos >= src.size())
			return fail();
		const char c = src[pos];
		if (c == '{')
			return parseObject(out);
		if (c == '[')
			return parseArray(out);
		if (c == '"')
		{
			if (!consume('"'))
				return false;
			out = AuthJson::Value();
			out.type = AuthJson::Value::Type::String;
			return parseStringInto(out.text);
		}
		if (c == 't')
		{
			if (!literal("true"))
				return false;
			out = AuthJson::Value();
			out.type = AuthJson::Value::Type::Bool;
			out.boolean = true;
			return true;
		}
		if (c == 'f')
		{
			if (!literal("false"))
				return false;
			out = AuthJson::Value();
			out.type = AuthJson::Value::Type::Bool;
			return true;
		}
		if (c == 'n')
		{
			if (!literal("null"))
				return false;
			out = AuthJson::Value();
			return true;
		}
		return parseNumber(out);
	}

	bool parseNumber(AuthJson::Value &out)
	{
		skipSpace();
		const std::size_t start = pos;
		while (pos < src.size())
		{
			const char c = src[pos];
			if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
				pos++;
			else
				break;
		}
		if (pos == start)
			return fail();
		const std::string text = src.substr(start, pos - start);
		char *end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		if (end == nullptr || *end != '\0')
			return fail();
		out = AuthJson::Value();
		out.type = AuthJson::Value::Type::Number;
		out.number = value;
		return true;
	}

	bool parseObject(AuthJson::Value &out)
	{
		if (!consume('{'))
			return false;
		out = AuthJson::Value();
		out.type = AuthJson::Value::Type::Object;
		if (tryConsume('}'))
			return true;
		for (;;)
		{
			AuthJson::Value key;
			if (!consume('"'))
				return false;
			key.type = AuthJson::Value::Type::String;
			if (!parseStringInto(key.text))
				return false;
			if (!consume(':'))
				return false;
			AuthJson::Value value;
			if (!parseValue(value))
				return false;
			out.object.emplace_back(key.text, std::move(value));
			if (tryConsume(','))
				continue;
			return consume('}');
		}
	}

	bool parseArray(AuthJson::Value &out)
	{
		if (!consume('['))
			return false;
		out = AuthJson::Value();
		out.type = AuthJson::Value::Type::Array;
		if (tryConsume(']'))
			return true;
		for (;;)
		{
			AuthJson::Value value;
			if (!parseValue(value))
				return false;
			out.array.push_back(std::move(value));
			if (tryConsume(','))
				continue;
			return consume(']');
		}
	}
};
} // namespace

bool AuthJson::parse(const std::string &document, Value &out)
{
	Parser parser(document);
	Value value;
	if (!parser.parseDocument(value))
		return false;
	out = std::move(value);
	return true;
}

std::string AuthJson::escape(const std::string &text)
{
	std::string out;
	out.reserve(text.size() + 8);
	for (const char c : text)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20)
			{
				char buffer[8];
				std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
				out += buffer;
			}
			else
			{
				out += c;
			}
		}
	}
	return out;
}
