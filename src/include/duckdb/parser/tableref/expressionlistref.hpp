//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/expressionlistref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
//! Represents an expression list as generated by a VALUES statement
class ExpressionListRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::EXPRESSION_LIST;

public:
	ExpressionListRef() : TableRef(TableReferenceType::EXPRESSION_LIST) {
	}

	//! Value list, only used for VALUES statement
	vector<vector<unique_ptr<ParsedExpression>>> values;
	//! Expected SQL types
	vector<LogicalType> expected_types;
	//! The set of expected names
	vector<string> expected_names;

public:
	string ToString() const override;
	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Serializes a blob into a ExpressionListRef
	void Serialize(FieldWriter &serializer) const override;
	//! Deserializes a blob back into a ExpressionListRef
	static unique_ptr<TableRef> Deserialize(FieldReader &source);

	void FormatSerialize(FormatSerializer &serializer) const override;
	static unique_ptr<TableRef> FormatDeserialize(FormatDeserializer &source);
};
} // namespace duckdb
