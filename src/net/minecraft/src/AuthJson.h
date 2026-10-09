#pragma once

#include <string>
#include <utility>
#include <vector>

// AuthJson.h -- the minimal JSON reader the Microsoft login flow needs
// (request bodies are built by hand with escape(), responses are parsed
// here). The MCP 1.2.5 tree carries a full J_Json* port, but its
// builder/selector surface is aimed at the stats syncher; this is a
// purpose-built value model for the auth endpoints: objects (ordered,
// linear lookup -- the documents are tiny), arrays, strings, numbers as
// double, bools and null.

namespace AuthJson
{
	class Value
	{
	public:
		enum class Type
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object
		};

		Type type = Type::Null;
		bool boolean = false;
		double number = 0.0;
		std::string text;                                 // String payload, decoded
		std::vector<Value> array;                         // Array payload
		std::vector<std::pair<std::string, Value>> object; // Object payload, ordered

		bool isNull() const { return type == Type::Null; }
		bool isString() const { return type == Type::String; }
		bool isNumber() const { return type == Type::Number; }
		bool isObject() const { return type == Type::Object; }

		const Value *member(const std::string &key) const
		{
			if (type != Type::Object)
				return nullptr;
			for (const auto &entry : object)
				if (entry.first == key)
					return &entry.second;
			return nullptr;
		}

		std::string textOr(const std::string &fallback) const
		{
			return type == Type::String ? text : fallback;
		}

		bool numberAsInteger(long long &out) const
		{
			if (type != Type::Number)
				return false;
			out = static_cast<long long>(number);
			return true;
		}
	};

	// Parses one complete JSON document; false (out untouched) on any
	// malformed input, trailing garbage included.
	bool parse(const std::string &document, Value &out);

	// Escapes text into a JSON string payload (no surrounding quotes) for
	// building request bodies.
	std::string escape(const std::string &text);
}
