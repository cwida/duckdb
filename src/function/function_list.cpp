#include "duckdb/function/function_list.hpp"

#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/geometry_functions.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar/map_functions.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/function/scalar/sequence_functions.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar/system_functions.hpp"
#include "duckdb/function/scalar/tablefilter_functions.hpp"
#include "duckdb/function/window/match_recognize_functions.hpp"
#include "duckdb/function/window/ranking_functions.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/function/window/value_functions.hpp"
#include "duckdb/function/scalar/comparison_functions.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

// Scalar Function
#define DUCKDB_SCALAR_FUNCTION_BASE(_PARAM, _NAME, _ALIAS_OF)                                                          \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories,                \
		    _PARAM::GetFunction, nullptr, nullptr, nullptr, nullptr, nullptr                                           \
	}
#define DUCKDB_SCALAR_FUNCTION(_PARAM) DUCKDB_SCALAR_FUNCTION_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_SCALAR_FUNCTION_ALIAS(_PARAM)                                                                           \
	DUCKDB_SCALAR_FUNCTION_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Scalar Function Set
#define DUCKDB_SCALAR_FUNCTION_SET_BASE(_PARAM, _NAME, _ALIAS_OF)                                                      \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories, nullptr,       \
		    _PARAM::GetFunctions, nullptr, nullptr, nullptr, nullptr                                                   \
	}
#define DUCKDB_SCALAR_FUNCTION_SET(_PARAM) DUCKDB_SCALAR_FUNCTION_SET_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_SCALAR_FUNCTION_SET_ALIAS(_PARAM)                                                                       \
	DUCKDB_SCALAR_FUNCTION_SET_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Aggregate Function
#define DUCKDB_AGGREGATE_FUNCTION_BASE(_PARAM, _NAME, _ALIAS_OF)                                                       \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories, nullptr,       \
		    nullptr, _PARAM::GetFunction, nullptr, nullptr, nullptr                                                    \
	}
#define DUCKDB_AGGREGATE_FUNCTION(_PARAM) DUCKDB_AGGREGATE_FUNCTION_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_AGGREGATE_FUNCTION_ALIAS(_PARAM)                                                                        \
	DUCKDB_AGGREGATE_FUNCTION_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Aggregate Function Set
#define DUCKDB_AGGREGATE_FUNCTION_SET_BASE(_PARAM, _NAME, _ALIAS_OF)                                                   \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories, nullptr,       \
		    nullptr, nullptr, _PARAM::GetFunctions, nullptr, nullptr                                                   \
	}
#define DUCKDB_AGGREGATE_FUNCTION_SET(_PARAM) DUCKDB_AGGREGATE_FUNCTION_SET_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_AGGREGATE_FUNCTION_SET_ALIAS(_PARAM)                                                                    \
	DUCKDB_AGGREGATE_FUNCTION_SET_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Window Function
#define DUCKDB_WINDOW_FUNCTION_BASE(_PARAM, _NAME, _ALIAS_OF)                                                          \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories, nullptr,       \
		    nullptr, nullptr, nullptr, _PARAM::GetFunction, nullptr                                                    \
	}
#define DUCKDB_WINDOW_FUNCTION(_PARAM) DUCKDB_WINDOW_FUNCTION_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_WINDOW_FUNCTION_ALIAS(_PARAM)                                                                           \
	DUCKDB_WINDOW_FUNCTION_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Window Function Set
#define DUCKDB_WINDOW_FUNCTION_SET_BASE(_PARAM, _NAME, _ALIAS_OF)                                                      \
	{                                                                                                                  \
		_NAME, _ALIAS_OF, _PARAM::Parameters, _PARAM::Description, _PARAM::Example, _PARAM::Categories, nullptr,       \
		    nullptr, nullptr, nullptr, nullptr, _PARAM::GetFunctions                                                   \
	}
#define DUCKDB_WINDOW_FUNCTION_SET(_PARAM) DUCKDB_WINDOW_FUNCTION_SET_BASE(_PARAM, _PARAM::Name, _PARAM::Name)
#define DUCKDB_WINDOW_FUNCTION_SET_ALIAS(_PARAM)                                                                       \
	DUCKDB_WINDOW_FUNCTION_SET_BASE(_PARAM::ALIAS, _PARAM::Name, _PARAM::ALIAS::Name)
// Final Function
#define FINAL_FUNCTION                                                                                                 \
	{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }

// this list is generated by scripts/generate_functions.py
static const StaticFunctionDefinition function[] = {
	DUCKDB_SCALAR_FUNCTION(OperatorNotEqualFun),
	DUCKDB_SCALAR_FUNCTION(NotLikeFun),
	DUCKDB_SCALAR_FUNCTION(NotILikeFun),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorModuloFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StIntersectsExtentFunAlias),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorMultiplyFun),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorAddFun),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorSubtractFun),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorFloatDivideFun),
	DUCKDB_SCALAR_FUNCTION_SET(OperatorIntegerDivideFun),
	DUCKDB_SCALAR_FUNCTION(OperatorLessThanFun),
	DUCKDB_SCALAR_FUNCTION(OperatorLessThanEqualsFun),
	DUCKDB_SCALAR_FUNCTION(OperatorEqualFun),
	DUCKDB_SCALAR_FUNCTION(OperatorGreaterThanFun),
	DUCKDB_SCALAR_FUNCTION(OperatorGreaterThanEqualsFun),
	DUCKDB_SCALAR_FUNCTION(IsDistinctFromFun),
	DUCKDB_SCALAR_FUNCTION(IsNotDistinctFromFun),
	DUCKDB_SCALAR_FUNCTION(BetweenFun),
	DUCKDB_SCALAR_FUNCTION(CastFun),
	DUCKDB_SCALAR_FUNCTION(BarrierFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressGeometryPointFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalCompressIntegralUbigintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalCompressIntegralUintegerFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalCompressIntegralUsmallintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalCompressIntegralUtinyintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringHugeintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringUbigintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringUhugeintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringUintegerFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringUsmallintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressStringUtinyintFun),
	DUCKDB_SCALAR_FUNCTION(InternalCompressedMaterializationCastFun),
	DUCKDB_SCALAR_FUNCTION(InternalDecompressGeometryPointFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralBigintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralHugeintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralIntegerFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralSmallintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralUbigintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralUhugeintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralUintegerFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressIntegralUsmallintFun),
	DUCKDB_SCALAR_FUNCTION_SET(InternalDecompressStringFun),
	DUCKDB_SCALAR_FUNCTION(TableFilterBloomFilterFun),
	DUCKDB_SCALAR_FUNCTION(TableFilterDynamicFun),
	DUCKDB_SCALAR_FUNCTION(TableFilterOptionalFun),
	DUCKDB_SCALAR_FUNCTION(TableFilterPrefixRangeFun),
	DUCKDB_SCALAR_FUNCTION(TableFilterSelectivityOptionalFun),
	DUCKDB_SCALAR_FUNCTION(TryRethrowFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(AddFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(AnyValueFun),
	DUCKDB_AGGREGATE_FUNCTION_SET_ALIAS(ArbitraryFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayCatFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayConcatFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayContainsFun),
	DUCKDB_SCALAR_FUNCTION_SET(ArrayExtractFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayHasFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayIndexofFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayIntersectFun),
	DUCKDB_SCALAR_FUNCTION_SET(ArrayLengthFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayPositionFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(ArrayResizeFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArraySelectFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayWhereFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ArrayZipFun),
	DUCKDB_SCALAR_FUNCTION_SET(BitLengthFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(CharLengthFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(CharacterLengthFun),
	DUCKDB_SCALAR_FUNCTION(CombineFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(CombineAggrFun),
	DUCKDB_SCALAR_FUNCTION(ConcatFun),
	DUCKDB_SCALAR_FUNCTION(ConcatWsFun),
	DUCKDB_SCALAR_FUNCTION(ConstantOrNullFun),
	DUCKDB_SCALAR_FUNCTION_SET(ContainsFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(CountFun),
	DUCKDB_AGGREGATE_FUNCTION(CountStarFun),
	DUCKDB_SCALAR_FUNCTION(CreateSortKeyFun),
	DUCKDB_WINDOW_FUNCTION(CumeDistFun),
	DUCKDB_SCALAR_FUNCTION(CurrentConnectionId),
	DUCKDB_SCALAR_FUNCTION(CurrentQueryId),
	DUCKDB_SCALAR_FUNCTION(CurrentTransactionId),
	DUCKDB_SCALAR_FUNCTION(CurrvalFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(DecimalAverageFun),
	DUCKDB_SCALAR_FUNCTION_SET(DecimalDivisionFun),
	DUCKDB_WINDOW_FUNCTION(DenseRankFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(DivideFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(EndsWithFun),
	DUCKDB_SCALAR_FUNCTION(ErrorFun),
	DUCKDB_WINDOW_FUNCTION(FillFun),
	DUCKDB_SCALAR_FUNCTION(FinalizeFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(FirstFun),
	DUCKDB_WINDOW_FUNCTION(FirstValueFun),
	DUCKDB_SCALAR_FUNCTION_SET(FmodFun),
	DUCKDB_SCALAR_FUNCTION(GetVariableFun),
	DUCKDB_SCALAR_FUNCTION(HasErrorFun),
	DUCKDB_SCALAR_FUNCTION(HivePartitionComponentFun),
	DUCKDB_SCALAR_FUNCTION(IlikeEscapeFun),
	DUCKDB_SCALAR_FUNCTION(InvokeFun),
	DUCKDB_WINDOW_FUNCTION(LagFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(LastFun),
	DUCKDB_WINDOW_FUNCTION(LastValueFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(LcaseFun),
	DUCKDB_WINDOW_FUNCTION(LeadFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(LenFun),
	DUCKDB_SCALAR_FUNCTION_SET(LengthFun),
	DUCKDB_SCALAR_FUNCTION_SET(LengthGraphemeFun),
	DUCKDB_SCALAR_FUNCTION(LikeEscapeFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ListCatFun),
	DUCKDB_SCALAR_FUNCTION(ListConcatFun),
	DUCKDB_SCALAR_FUNCTION(ListContainsFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(ListElementFun),
	DUCKDB_SCALAR_FUNCTION_SET(ListExtractFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ListHasFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(ListIndexofFun),
	DUCKDB_SCALAR_FUNCTION(ListIntersectFun),
	DUCKDB_SCALAR_FUNCTION(ListPositionFun),
	DUCKDB_SCALAR_FUNCTION_SET(ListResizeFun),
	DUCKDB_SCALAR_FUNCTION(ListSelectFun),
	DUCKDB_SCALAR_FUNCTION(ListWhereFun),
	DUCKDB_SCALAR_FUNCTION(ListZipFun),
	DUCKDB_SCALAR_FUNCTION(LowerFun),
	DUCKDB_SCALAR_FUNCTION(MapContainsFun),
	DUCKDB_WINDOW_FUNCTION(MatchRecognizeFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(MaxFun),
	DUCKDB_SCALAR_FUNCTION_SET(MD5Fun),
	DUCKDB_SCALAR_FUNCTION_SET(MD5NumberFun),
	DUCKDB_AGGREGATE_FUNCTION_SET(MinFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(ModFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(MultiplyFun),
	DUCKDB_SCALAR_FUNCTION(NextvalFun),
	DUCKDB_SCALAR_FUNCTION(NFCNormalizeFun),
	DUCKDB_SCALAR_FUNCTION(NotIlikeEscapeFun),
	DUCKDB_SCALAR_FUNCTION(NotLikeEscapeFun),
	DUCKDB_WINDOW_FUNCTION(NthValueFun),
	DUCKDB_WINDOW_FUNCTION(NtileFun),
	DUCKDB_SCALAR_FUNCTION_SET(OctetLengthFun),
	DUCKDB_SCALAR_FUNCTION_SET(OverlayFun),
	DUCKDB_SCALAR_FUNCTION(ParseLogMessage),
	DUCKDB_SCALAR_FUNCTION(PathJoinFun),
	DUCKDB_WINDOW_FUNCTION(PercentRankFun),
	DUCKDB_SCALAR_FUNCTION(PrefixFun),
	DUCKDB_WINDOW_FUNCTION(RankFun),
	DUCKDB_WINDOW_FUNCTION_ALIAS(RankDenseFun),
	DUCKDB_SCALAR_FUNCTION(RegexpEscapeFun),
	DUCKDB_SCALAR_FUNCTION_SET(RegexpExtractFun),
	DUCKDB_SCALAR_FUNCTION_SET(RegexpExtractAllFun),
	DUCKDB_SCALAR_FUNCTION_SET(RegexpFun),
	DUCKDB_SCALAR_FUNCTION_SET(RegexpMatchesFun),
	DUCKDB_SCALAR_FUNCTION_SET(RegexpReplaceFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(RegexpSplitToArrayFun),
	DUCKDB_SCALAR_FUNCTION(RemapStructFun),
	DUCKDB_SCALAR_FUNCTION(RowFun),
	DUCKDB_WINDOW_FUNCTION(RowNumberFun),
	DUCKDB_SCALAR_FUNCTION_SET(SetvalFun),
	DUCKDB_SCALAR_FUNCTION_SET(SHA1Fun),
	DUCKDB_SCALAR_FUNCTION_SET(SHA256Fun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(SplitFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StAsbinaryFun),
	DUCKDB_SCALAR_FUNCTION(StAstextFun),
	DUCKDB_SCALAR_FUNCTION(StAswkbFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StAswktFun),
	DUCKDB_SCALAR_FUNCTION(StCrsFun),
	DUCKDB_SCALAR_FUNCTION(StGeomfromwkbFun),
	DUCKDB_SCALAR_FUNCTION(StIntersectsExtentFun),
	DUCKDB_SCALAR_FUNCTION(StSetcrsFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StrSplitFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(StrSplitRegexFun),
	DUCKDB_SCALAR_FUNCTION_SET(StrfTimeFun),
	DUCKDB_SCALAR_FUNCTION(StringSplitFun),
	DUCKDB_SCALAR_FUNCTION_SET(StringSplitRegexFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StringToArrayFun),
	DUCKDB_SCALAR_FUNCTION(StripAccentsFun),
	DUCKDB_SCALAR_FUNCTION(StrlenFun),
	DUCKDB_SCALAR_FUNCTION_SET(StrpTimeFun),
	DUCKDB_SCALAR_FUNCTION(StructConcatFun),
	DUCKDB_SCALAR_FUNCTION(StructContainsFun),
	DUCKDB_SCALAR_FUNCTION_SET(StructExtractFun),
	DUCKDB_SCALAR_FUNCTION(StructExtractAtFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StructHasFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(StructIndexofFun),
	DUCKDB_SCALAR_FUNCTION(StructPackFun),
	DUCKDB_SCALAR_FUNCTION(StructPositionFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(SubstrFun),
	DUCKDB_SCALAR_FUNCTION_SET(SubstringFun),
	DUCKDB_SCALAR_FUNCTION_SET(SubstringGraphemeFun),
	DUCKDB_SCALAR_FUNCTION_SET_ALIAS(SubtractFun),
	DUCKDB_SCALAR_FUNCTION(SuffixFun),
	DUCKDB_SCALAR_FUNCTION_SET(ToAggregateStateFun),
	DUCKDB_SCALAR_FUNCTION_SET(TryStrpTimeFun),
	DUCKDB_SCALAR_FUNCTION_ALIAS(UcaseFun),
	DUCKDB_SCALAR_FUNCTION(UpperFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantArrayLengthFun),
	DUCKDB_SCALAR_FUNCTION(VariantComparatorFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantContainsFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantExistsFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantExtractFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantExtractStringFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantKeysFun),
	DUCKDB_SCALAR_FUNCTION(VariantNormalizeFun),
	DUCKDB_SCALAR_FUNCTION_SET(VariantTypeFun),
	DUCKDB_SCALAR_FUNCTION(VariantTypeofFun),
	DUCKDB_SCALAR_FUNCTION(VertexExtractFun),
	DUCKDB_SCALAR_FUNCTION_SET(WriteLogFun),
	DUCKDB_SCALAR_FUNCTION(ConcatOperatorFun),
	DUCKDB_SCALAR_FUNCTION(LikeFun),
	DUCKDB_SCALAR_FUNCTION(ILikeFun),
	DUCKDB_SCALAR_FUNCTION(GlobPatternFun),
	FINAL_FUNCTION
};

const StaticFunctionDefinition *FunctionList::GetInternalFunctionList() {
	return function;
}

} // namespace duckdb
