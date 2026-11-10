//          Copyright Malte Skarupke 2016.
// Distributed under the Boost Software License, Version 1.0.
//    (See http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstdint>
#include <algorithm>
#include <type_traits>
#include <tuple>
#include <utility>

#include "pdqsort.h"
#include "vergesort.h"

namespace duckdb_ska_sort {

namespace detail
{
template<typename count_type, typename It, typename OutIt, typename ExtractKey>
void counting_sort_impl(It begin, It end, OutIt out_begin, ExtractKey && extract_key)
{
    count_type counts[256] = {};
    for (It it = begin; it != end; ++it)
    {
        ++counts[extract_key(*it)];
    }
    count_type nonzero = 0;
    count_type total = 0;
    for (count_type * it = counts, * end_it = counts + 256; it != end_it; ++it)
    {
        count_type old_count = *it;
        nonzero += old_count != 0;
        *it = total;
        total += old_count;
    }
    if (nonzero == 1) {
        return; // LNK: skip moving data if all belong to same bucket
    }
    for (; begin != end; ++begin)
    {
        std::uint8_t key = extract_key(*begin);
        out_begin[counts[key]] = std::move(*begin);
        ++counts[key];
    }
}
template<typename It, typename OutIt, typename ExtractKey>
void counting_sort_impl(It begin, It end, OutIt out_begin, ExtractKey && extract_key)
{
    counting_sort_impl<std::uint64_t>(begin, end, out_begin, extract_key);
}
inline bool to_unsigned_or_bool(bool b)
{
    return b;
}
inline unsigned char to_unsigned_or_bool(unsigned char c)
{
    return c;
}
inline unsigned char to_unsigned_or_bool(signed char c)
{
    return static_cast<unsigned char>(c) + 128;
}
inline unsigned char to_unsigned_or_bool(char c)
{
    return static_cast<unsigned char>(c);
}
inline std::uint16_t to_unsigned_or_bool(char16_t c)
{
    return static_cast<std::uint16_t>(c);
}
inline std::uint32_t to_unsigned_or_bool(char32_t c)
{
    return static_cast<std::uint32_t>(c);
}
inline std::uint32_t to_unsigned_or_bool(wchar_t c)
{
    return static_cast<std::uint32_t>(c);
}
inline unsigned short to_unsigned_or_bool(short i)
{
    return static_cast<unsigned short>(i) + static_cast<unsigned short>(1 << (sizeof(short) * 8 - 1));
}
inline unsigned short to_unsigned_or_bool(unsigned short i)
{
    return i;
}
inline unsigned int to_unsigned_or_bool(int i)
{
    return static_cast<unsigned int>(i) + static_cast<unsigned int>(1 << (sizeof(int) * 8 - 1));
}
inline unsigned int to_unsigned_or_bool(unsigned int i)
{
    return i;
}
inline unsigned long to_unsigned_or_bool(long l)
{
    return static_cast<unsigned long>(l) + static_cast<unsigned long>(1l << (sizeof(long) * 8 - 1));
}
inline unsigned long to_unsigned_or_bool(unsigned long l)
{
    return l;
}
inline unsigned long long to_unsigned_or_bool(long long l)
{
    return static_cast<unsigned long long>(l) + static_cast<unsigned long long>(1ll << (sizeof(long long) * 8 - 1));
}
inline unsigned long long to_unsigned_or_bool(unsigned long long l)
{
    return l;
}
inline std::uint32_t to_unsigned_or_bool(float f)
{
    union
    {
        float f;
        std::uint32_t u;
    } as_union = { f };
    std::uint32_t sign_bit = static_cast<std::uint32_t>(-std::int32_t(as_union.u >> 31));
    return as_union.u ^ (sign_bit | 0x80000000);
}
inline std::uint64_t to_unsigned_or_bool(double f)
{
    union
    {
        double d;
        std::uint64_t u;
    } as_union = { f };
    std::uint64_t sign_bit = static_cast<std::uint64_t>(-std::int64_t(as_union.u >> 63));
    return as_union.u ^ (sign_bit | 0x8000000000000000);
}
template<typename T>
inline size_t to_unsigned_or_bool(T * ptr)
{
    return reinterpret_cast<size_t>(ptr);
}

// template<size_t>
// struct SizedRadixSorter;
//
// template<>
// struct SizedRadixSorter<1>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         counting_sort_impl(begin, end, buffer_begin, [&](typename std::remove_reference<decltype(*begin)>::type & o)
//         {
//             return to_unsigned_or_bool(extract_key(o));
//         });
//         return true;
//     }
//
//     static constexpr size_t pass_count = 2;
// };
// template<>
// struct SizedRadixSorter<2>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         size_t num_elements = end - begin;
//         if (num_elements <= (1ll << 32))
//             return sort_inline<uint32_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//         else
//             return sort_inline<uint64_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//     }
//
//     template<typename count_type, typename It, typename OutIt, typename ExtractKey>
//     static bool sort_inline(It begin, It end, OutIt out_begin, OutIt out_end, ExtractKey && extract_key)
//     {
//         count_type counts0[256] = {};
//         count_type counts1[256] = {};
//
//         for (It it = begin; it != end; ++it)
//         {
//             uint16_t key = to_unsigned_or_bool(extract_key(*it));
//             ++counts0[key & 0xff];
//             ++counts1[(key >> 8) & 0xff];
//         }
//         count_type total0 = 0;
//         count_type total1 = 0;
//         for (int i = 0; i < 256; ++i)
//         {
//             count_type old_count0 = counts0[i];
//             count_type old_count1 = counts1[i];
//             counts0[i] = total0;
//             counts1[i] = total1;
//             total0 += old_count0;
//             total1 += old_count1;
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it));
//             out_begin[counts0[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 8;
//             begin[counts1[key]++] = std::move(*it);
//         }
//         return false;
//     }
//
//     static constexpr size_t pass_count = 3;
// };
// template<>
// struct SizedRadixSorter<4>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         size_t num_elements = end - begin;
//         if (num_elements <= (1ll << 32))
//             return sort_inline<uint32_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//         else
//             return sort_inline<uint64_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//     }
//     template<typename count_type, typename It, typename OutIt, typename ExtractKey>
//     static bool sort_inline(It begin, It end, OutIt out_begin, OutIt out_end, ExtractKey && extract_key)
//     {
//         count_type counts0[256] = {};
//         count_type counts1[256] = {};
//         count_type counts2[256] = {};
//         count_type counts3[256] = {};
//
//         for (It it = begin; it != end; ++it)
//         {
//             uint32_t key = to_unsigned_or_bool(extract_key(*it));
//             ++counts0[key & 0xff];
//             ++counts1[(key >> 8) & 0xff];
//             ++counts2[(key >> 16) & 0xff];
//             ++counts3[(key >> 24) & 0xff];
//         }
//         count_type total0 = 0;
//         count_type total1 = 0;
//         count_type total2 = 0;
//         count_type total3 = 0;
//         for (int i = 0; i < 256; ++i)
//         {
//             count_type old_count0 = counts0[i];
//             count_type old_count1 = counts1[i];
//             count_type old_count2 = counts2[i];
//             count_type old_count3 = counts3[i];
//             counts0[i] = total0;
//             counts1[i] = total1;
//             counts2[i] = total2;
//             counts3[i] = total3;
//             total0 += old_count0;
//             total1 += old_count1;
//             total2 += old_count2;
//             total3 += old_count3;
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it));
//             out_begin[counts0[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 8;
//             begin[counts1[key]++] = std::move(*it);
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 16;
//             out_begin[counts2[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 24;
//             begin[counts3[key]++] = std::move(*it);
//         }
//         return false;
//     }
//
//     static constexpr size_t pass_count = 5;
// };
// template<>
// struct SizedRadixSorter<8>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         size_t num_elements = end - begin;
//         if (num_elements <= (1ll << 32))
//             return sort_inline<uint32_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//         else
//             return sort_inline<uint64_t>(begin, end, buffer_begin, buffer_begin + num_elements, extract_key);
//     }
//     template<typename count_type, typename It, typename OutIt, typename ExtractKey>
//     static bool sort_inline(It begin, It end, OutIt out_begin, OutIt out_end, ExtractKey && extract_key)
//     {
//         count_type counts0[256] = {};
//         count_type counts1[256] = {};
//         count_type counts2[256] = {};
//         count_type counts3[256] = {};
//         count_type counts4[256] = {};
//         count_type counts5[256] = {};
//         count_type counts6[256] = {};
//         count_type counts7[256] = {};
//
//         for (It it = begin; it != end; ++it)
//         {
//             uint64_t key = to_unsigned_or_bool(extract_key(*it));
//             ++counts0[key & 0xff];
//             ++counts1[(key >> 8) & 0xff];
//             ++counts2[(key >> 16) & 0xff];
//             ++counts3[(key >> 24) & 0xff];
//             ++counts4[(key >> 32) & 0xff];
//             ++counts5[(key >> 40) & 0xff];
//             ++counts6[(key >> 48) & 0xff];
//             ++counts7[(key >> 56) & 0xff];
//         }
//         count_type total0 = 0;
//         count_type total1 = 0;
//         count_type total2 = 0;
//         count_type total3 = 0;
//         count_type total4 = 0;
//         count_type total5 = 0;
//         count_type total6 = 0;
//         count_type total7 = 0;
//         for (int i = 0; i < 256; ++i)
//         {
//             count_type old_count0 = counts0[i];
//             count_type old_count1 = counts1[i];
//             count_type old_count2 = counts2[i];
//             count_type old_count3 = counts3[i];
//             count_type old_count4 = counts4[i];
//             count_type old_count5 = counts5[i];
//             count_type old_count6 = counts6[i];
//             count_type old_count7 = counts7[i];
//             counts0[i] = total0;
//             counts1[i] = total1;
//             counts2[i] = total2;
//             counts3[i] = total3;
//             counts4[i] = total4;
//             counts5[i] = total5;
//             counts6[i] = total6;
//             counts7[i] = total7;
//             total0 += old_count0;
//             total1 += old_count1;
//             total2 += old_count2;
//             total3 += old_count3;
//             total4 += old_count4;
//             total5 += old_count5;
//             total6 += old_count6;
//             total7 += old_count7;
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it));
//             out_begin[counts0[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 8;
//             begin[counts1[key]++] = std::move(*it);
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 16;
//             out_begin[counts2[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 24;
//             begin[counts3[key]++] = std::move(*it);
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 32;
//             out_begin[counts4[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 40;
//             begin[counts5[key]++] = std::move(*it);
//         }
//         for (It it = begin; it != end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 48;
//             out_begin[counts6[key]++] = std::move(*it);
//         }
//         for (OutIt it = out_begin; it != out_end; ++it)
//         {
//             std::uint8_t key = to_unsigned_or_bool(extract_key(*it)) >> 56;
//             begin[counts7[key]++] = std::move(*it);
//         }
//         return false;
//     }
//
//     static constexpr size_t pass_count = 9;
// };
//
// template<typename>
// struct RadixSorter;
// template<>
// struct RadixSorter<bool>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         size_t false_count = 0;
//         for (It it = begin; it != end; ++it)
//         {
//             if (!extract_key(*it))
//                 ++false_count;
//         }
//         size_t true_position = false_count;
//         false_count = 0;
//         for (; begin != end; ++begin)
//         {
//             if (extract_key(*begin))
//                 buffer_begin[true_position++] = std::move(*begin);
//             else
//                 buffer_begin[false_count++] = std::move(*begin);
//         }
//         return true;
//     }
//
//     static constexpr size_t pass_count = 2;
// };
// template<>
// struct RadixSorter<signed char> : SizedRadixSorter<sizeof(signed char)>
// {
// };
// template<>
// struct RadixSorter<unsigned char> : SizedRadixSorter<sizeof(unsigned char)>
// {
// };
// template<>
// struct RadixSorter<signed short> : SizedRadixSorter<sizeof(signed short)>
// {
// };
// template<>
// struct RadixSorter<unsigned short> : SizedRadixSorter<sizeof(unsigned short)>
// {
// };
// template<>
// struct RadixSorter<signed int> : SizedRadixSorter<sizeof(signed int)>
// {
// };
// template<>
// struct RadixSorter<unsigned int> : SizedRadixSorter<sizeof(unsigned int)>
// {
// };
// template<>
// struct RadixSorter<signed long> : SizedRadixSorter<sizeof(signed long)>
// {
// };
// template<>
// struct RadixSorter<unsigned long> : SizedRadixSorter<sizeof(unsigned long)>
// {
// };
// template<>
// struct RadixSorter<signed long long> : SizedRadixSorter<sizeof(signed long long)>
// {
// };
// template<>
// struct RadixSorter<unsigned long long> : SizedRadixSorter<sizeof(unsigned long long)>
// {
// };
// template<>
// struct RadixSorter<float> : SizedRadixSorter<sizeof(float)>
// {
// };
// template<>
// struct RadixSorter<double> : SizedRadixSorter<sizeof(double)>
// {
// };
// template<>
// struct RadixSorter<char> : SizedRadixSorter<sizeof(char)>
// {
// };
// template<>
// struct RadixSorter<wchar_t> : SizedRadixSorter<sizeof(wchar_t)>
// {
// };
// template<>
// struct RadixSorter<char16_t> : SizedRadixSorter<sizeof(char16_t)>
// {
// };
// template<>
// struct RadixSorter<char32_t> : SizedRadixSorter<sizeof(char32_t)>
// {
// };
// template<typename K, typename V>
// struct RadixSorter<std::pair<K, V>>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         bool first_result = RadixSorter<V>::sort(begin, end, buffer_begin, [&](typename std::remove_reference<decltype(*begin)>::type & o)
//         {
//             return extract_key(o).second;
//         });
//         auto extract_first = [&](typename std::remove_reference<decltype(*begin)>::type & o)
//         {
//             return extract_key(o).first;
//         };
//
//         if (first_result)
//         {
//             return !RadixSorter<K>::sort(buffer_begin, buffer_begin + (end - begin), begin, extract_first);
//         }
//         else
//         {
//             return RadixSorter<K>::sort(begin, end, buffer_begin, extract_first);
//         }
//     }
//
//     static constexpr size_t pass_count = RadixSorter<K>::pass_count + RadixSorter<V>::pass_count;
// };
// template<typename K, typename V>
// struct RadixSorter<const std::pair<K, V> &>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         bool first_result = RadixSorter<V>::sort(begin, end, buffer_begin, [&](typename std::remove_reference<decltype(*begin)>::type & o) -> const V &
//         {
//             return extract_key(o).second;
//         });
//         auto extract_first = [&](typename std::remove_reference<decltype(*begin)>::type & o) -> const K &
//         {
//             return extract_key(o).first;
//         };
//
//         if (first_result)
//         {
//             return !RadixSorter<K>::sort(buffer_begin, buffer_begin + (end - begin), begin, extract_first);
//         }
//         else
//         {
//             return RadixSorter<K>::sort(begin, end, buffer_begin, extract_first);
//         }
//     }
//
//     static constexpr size_t pass_count = RadixSorter<K>::pass_count + RadixSorter<V>::pass_count;
// };
// template<size_t I, size_t S, typename Tuple>
// struct TupleRadixSorter
// {
//     using NextSorter = TupleRadixSorter<I + 1, S, Tuple>;
//     using ThisSorter = RadixSorter<typename std::tuple_element<I, Tuple>::type>;
//
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt out_begin, OutIt out_end, ExtractKey && extract_key)
//     {
//         bool which = NextSorter::sort(begin, end, out_begin, out_end, extract_key);
//         auto extract_i = [&](typename std::remove_reference<decltype(*begin)>::type & o)
//         {
//             return std::get<I>(extract_key(o));
//         };
//         if (which)
//             return !ThisSorter::sort(out_begin, out_end, begin, extract_i);
//         else
//             return ThisSorter::sort(begin, end, out_begin, extract_i);
//     }
//
//     static constexpr size_t pass_count = ThisSorter::pass_count + NextSorter::pass_count;
// };
// template<size_t I, size_t S, typename Tuple>
// struct TupleRadixSorter<I, S, const Tuple &>
// {
//     using NextSorter = TupleRadixSorter<I + 1, S, const Tuple &>;
//     using ThisSorter = RadixSorter<typename std::tuple_element<I, Tuple>::type>;
//
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt out_begin, OutIt out_end, ExtractKey && extract_key)
//     {
//         bool which = NextSorter::sort(begin, end, out_begin, out_end, extract_key);
//         auto extract_i = [&](typename std::remove_reference<decltype(*begin)>::type & o) -> typename std::tuple_element<I, Tuple>::type const &
//         {
//             return std::get<I>(extract_key(o));
//         };
//         if (which)
//             return !ThisSorter::sort(out_begin, out_end, begin, extract_i);
//         else
//             return ThisSorter::sort(begin, end, out_begin, extract_i);
//     }
//
//     static constexpr size_t pass_count = ThisSorter::pass_count + NextSorter::pass_count;
// };
// template<size_t I, typename Tuple>
// struct TupleRadixSorter<I, I, Tuple>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It, It, OutIt, OutIt, ExtractKey &&)
//     {
//         return false;
//     }
//
//     static constexpr size_t pass_count = 0;
// };
// template<size_t I, typename Tuple>
// struct TupleRadixSorter<I, I, const Tuple &>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It, It, OutIt, OutIt, ExtractKey &&)
//     {
//         return false;
//     }
//
//     static constexpr size_t pass_count = 0;
// };
//
// template<typename... Args>
// struct RadixSorter<std::tuple<Args...>>
// {
//     using SorterImpl = TupleRadixSorter<0, sizeof...(Args), std::tuple<Args...>>;
//
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         return SorterImpl::sort(begin, end, buffer_begin, buffer_begin + (end - begin), extract_key);
//     }
//
//     static constexpr size_t pass_count = SorterImpl::pass_count;
// };
//
// template<typename... Args>
// struct RadixSorter<const std::tuple<Args...> &>
// {
//     using SorterImpl = TupleRadixSorter<0, sizeof...(Args), const std::tuple<Args...> &>;
//
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         return SorterImpl::sort(begin, end, buffer_begin, buffer_begin + (end - begin), extract_key);
//     }
//
//     static constexpr size_t pass_count = SorterImpl::pass_count;
// };
//
// template<typename T, size_t S>
// struct RadixSorter<std::array<T, S>>
// {
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         auto buffer_end = buffer_begin + (end - begin);
//         bool which = false;
//         for (size_t i = S; i > 0; --i)
//         {
//             auto i_copy = i - 1;
//             auto extract_i = [&, i_copy](typename std::remove_reference<decltype(*begin)>::type & o)
//             {
//                 return extract_key(o)[i_copy];
//             };
//             if (which)
//                 which = !RadixSorter<T>::sort(buffer_begin, buffer_end, begin, extract_i);
//             else
//                 which = RadixSorter<T>::sort(begin, end, buffer_begin, extract_i);
//         }
//         return which;
//     }
//
//     static constexpr size_t pass_count = RadixSorter<T>::pass_count * S;
// };
//
// template<typename T>
// struct RadixSorter<const T> : RadixSorter<T>
// {
// };
// template<typename T>
// struct RadixSorter<T &> : RadixSorter<const T &>
// {
// };
// template<typename T>
// struct RadixSorter<T &&> : RadixSorter<T>
// {
// };
// template<typename T>
// struct RadixSorter<const T &> : RadixSorter<T>
// {
// };
// template<typename T>
// struct RadixSorter<const T &&> : RadixSorter<T>
// {
// };
//
// struct ExampleStructA { int i; };
// struct ExampleStructB { float f; };
// inline int to_radix_sort_key(ExampleStructA a) { return a.i; }
// inline float to_radix_sort_key(ExampleStructB b) { return b.f; }
//
// template<typename T, typename Enable = void>
// struct FallbackRadixSorter : RadixSorter<decltype(to_radix_sort_key(std::declval<T>()))>
// {
//     using base = RadixSorter<decltype(to_radix_sort_key(std::declval<T>()))>;
//
//     template<typename It, typename OutIt, typename ExtractKey>
//     static bool sort(It begin, It end, OutIt buffer_begin, ExtractKey && extract_key)
//     {
//         return base::sort(begin, end, buffer_begin, [&](typename std::remove_reference<decltype(*begin)>::type & a)
//         {
//             return to_radix_sort_key(extract_key(a));
//         });
//     }
// };

template<typename T>
struct has_subscript_operator_impl
{
    template<typename U>
    static auto test(int) -> decltype(std::declval<U>()[0], std::true_type());
    template<typename>
    static std::false_type test(...);

    typedef decltype(test<T>(0)) type;
};

template<typename T>
struct has_subscript_operator : has_subscript_operator_impl<T>::type
{
};

// template<typename T>
// struct FallbackRadixSorter<T, typename std::enable_if<has_subscript_operator<T>::value>::type> : RadixSorter<typename std::decay<decltype(std::declval<T>()[0])>::type>
// {
// };
//
// template<typename T>
// struct RadixSorter : FallbackRadixSorter<T>
// {
// };
//
// template<typename T>
// struct RadixSortPassCount
// {
//     static const size_t value = RadixSorter<T>::pass_count;
// };

template<typename It, typename Func>
inline void unroll_loop_four_times(It begin, size_t iteration_count, Func && to_call)
{
    size_t loop_count = iteration_count / 4;
    size_t remainder_count = iteration_count - loop_count * 4;
    for (; loop_count > 0; --loop_count)
    {
        to_call(begin);
        ++begin;
        to_call(begin);
        ++begin;
        to_call(begin);
        ++begin;
        to_call(begin);
        ++begin;
    }
    switch(remainder_count)
    {
    case 3:
        to_call(begin);
        ++begin;
        DUCKDB_EXPLICIT_FALLTHROUGH;
    case 2:
        to_call(begin);
        ++begin;
        DUCKDB_EXPLICIT_FALLTHROUGH;
    case 1:
        to_call(begin);
    }
}

template<typename It, typename F>
inline It custom_std_partition(It begin, It end, F && func)
{
    for (;; ++begin)
    {
        if (begin == end)
            return end;
        if (!func(*begin))
            break;
    }
    It it = begin;
    for(++it; it != end; ++it)
    {
        if (!func(*it))
            continue;

        std::iter_swap(begin, it);
        ++begin;
    }
    return begin;
}

struct PartitionInfo
{
    PartitionInfo()
        : count(0)
    {
    }

    union
    {
        size_t count;
        size_t offset;
    };
    size_t next_offset;
};

template<size_t>
struct UnsignedForSize;
template<>
struct UnsignedForSize<1>
{
    typedef uint8_t type;
};
template<>
struct UnsignedForSize<2>
{
    typedef uint16_t type;
};
template<>
struct UnsignedForSize<4>
{
    typedef uint32_t type;
};
template<>
struct UnsignedForSize<8>
{
    typedef uint64_t type;
};
template<typename T>
struct SubKey;
template<size_t Size>
struct SizedSubKey
{
    template<typename T>
    static typename UnsignedForSize<Size>::type sub_key(T && value, void *)
    {
        return to_unsigned_or_bool(value);
    }

    typedef SubKey<void> next;

    using sub_key_type = typename UnsignedForSize<Size>::type;
};
template<typename T>
struct SubKey<const T> : SubKey<T>
{
};
template<typename T>
struct SubKey<T &> : SubKey<T>
{
};
template<typename T>
struct SubKey<T &&> : SubKey<T>
{
};
template<typename T>
struct SubKey<const T &> : SubKey<T>
{
};
template<typename T>
struct SubKey<const T &&> : SubKey<T>
{
};
// First, ensure this void_t helper is near the top of the file:
template<typename...> struct void_type { typedef void type; };
template<typename... Ts> using void_t = typename void_type<Ts...>::type;

// Primary template declaration
template<typename T, typename = void>
struct FallbackSubKey;

// Specialization 1: For types with to_radix_sort_key
template<typename T>
struct FallbackSubKey<T, void_t<decltype(to_radix_sort_key(std::declval<T>()))>>
    : SubKey<decltype(to_radix_sort_key(std::declval<T>()))>
{
    using base = SubKey<decltype(to_radix_sort_key(std::declval<T>()))>;

    template<typename U>
    static typename base::sub_key_type sub_key(U && value, void * data)
    {
        return base::sub_key(to_radix_sort_key(value), data);
    }
};

// Specialization 2: For types that can be converted to unsigned/bool
template<typename T>
struct FallbackSubKey<T, typename std::enable_if<
    !has_subscript_operator<T>::value && // Only if no subscript operator
    std::is_same<
        void_t<decltype(to_unsigned_or_bool(std::declval<T>()))>,
        void
    >::value
>::type>
    : SubKey<decltype(to_unsigned_or_bool(std::declval<T>()))>
{
};

// Specialization 3: For types with subscript operator (replaces both previous definitions)
template<typename T>
struct FallbackSubKey<T, typename std::enable_if<has_subscript_operator<T>::value>::type>
    : SubKey<typename std::decay<decltype(std::declval<T>()[0])>::type>
{
};

// The existing SubKey definition should remain unchanged:
template<typename T>
struct SubKey : FallbackSubKey<T>
{
};
template<>
struct SubKey<bool>
{
    template<typename T>
    static bool sub_key(T && value, void *)
    {
        return value;
    }

    typedef SubKey<void> next;

    using sub_key_type = bool;
};
template<>
struct SubKey<void>;
template<>
struct SubKey<unsigned char> : SizedSubKey<sizeof(unsigned char)>
{
};
template<>
struct SubKey<unsigned short> : SizedSubKey<sizeof(unsigned short)>
{
};
template<>
struct SubKey<unsigned int> : SizedSubKey<sizeof(unsigned int)>
{
};
template<>
struct SubKey<unsigned long> : SizedSubKey<sizeof(unsigned long)>
{
};
template<>
struct SubKey<unsigned long long> : SizedSubKey<sizeof(unsigned long long)>
{
};
template<typename T>
struct SubKey<T *> : SizedSubKey<sizeof(T *)>
{
};
template<typename F, typename S, typename Current>
struct PairSecondSubKey : Current
{
    static typename Current::sub_key_type sub_key(const std::pair<F, S> & value, void * sort_data)
    {
        return Current::sub_key(value.second, sort_data);
    }

    typedef typename std::conditional<std::is_same<SubKey<void>, typename Current::next>::value, SubKey<void>, PairSecondSubKey<F, S, typename Current::next>>::type next;
};
template<typename F, typename S, typename Current>
struct PairFirstSubKey : Current
{
    static typename Current::sub_key_type sub_key(const std::pair<F, S> & value, void * sort_data)
    {
        return Current::sub_key(value.first, sort_data);
    }

    typedef typename std::conditional<std::is_same<SubKey<void>, typename Current::next>::value, PairSecondSubKey<F, S, SubKey<S>>, PairFirstSubKey<F, S, typename Current::next>>::type next;
};
template<typename F, typename S>
struct SubKey<std::pair<F, S>> : PairFirstSubKey<F, S, SubKey<F>>
{
};
template<size_t Index, typename First, typename... More>
struct TypeAt : TypeAt<Index - 1, More..., void>
{
};
template<typename First, typename... More>
struct TypeAt<0, First, More...>
{
    typedef First type;
};

// template<size_t Index, typename Current, typename First, typename... More>
// struct TupleSubKey;
//
// template<size_t Index, typename Next, typename First, typename... More>
// struct NextTupleSubKey
// {
//     typedef TupleSubKey<Index, Next, First, More...> type;
// };
// template<size_t Index, typename First, typename Second, typename... More>
// struct NextTupleSubKey<Index, SubKey<void>, First, Second, More...>
// {
//     typedef TupleSubKey<Index + 1, SubKey<Second>, Second, More...> type;
// };
// template<size_t Index, typename First>
// struct NextTupleSubKey<Index, SubKey<void>, First>
// {
//     typedef SubKey<void> type;
// };
//
// template<size_t Index, typename Current, typename First, typename... More>
// struct TupleSubKey : Current
// {
//     template<typename Tuple>
//     static typename Current::sub_key_type sub_key(const Tuple & value, void * sort_data)
//     {
//         return Current::sub_key(std::get<Index>(value), sort_data);
//     }
//
//     typedef typename NextTupleSubKey<Index, typename Current::next, First, More...>::type next;
// };
// template<size_t Index, typename Current, typename First>
// struct TupleSubKey<Index, Current, First> : Current
// {
//     template<typename Tuple>
//     static typename Current::sub_key_type sub_key(const Tuple & value, void * sort_data)
//     {
//         return Current::sub_key(std::get<Index>(value), sort_data);
//     }
//
//     typedef typename NextTupleSubKey<Index, typename Current::next, First>::type next;
// };
// template<typename First, typename... More>
// struct SubKey<std::tuple<First, More...>> : TupleSubKey<0, SubKey<First>, First, More...>
// {
// };
//
// struct BaseListSortData
// {
//     size_t current_index;
//     size_t recursion_limit;
//     void * next_sort_data;
// };
// template<typename It, typename ExtractKey>
// struct ListSortData : BaseListSortData
// {
//     void (*next_sort)(It, It, size_t, ExtractKey &, void *);
// };
//
// template<typename CurrentSubKey, typename T>
// struct ListElementSubKey : SubKey<typename std::decay<decltype(std::declval<T>()[0])>::type>
// {
//     using base = SubKey<typename std::decay<decltype(std::declval<T>()[0])>::type>;
//
//     typedef ListElementSubKey next;
//
//     template<typename U>
//     static typename base::sub_key_type sub_key(U && value, void * sort_data)
//     {
//         BaseListSortData * list_sort_data = static_cast<BaseListSortData *>(sort_data);
//         const T & list = CurrentSubKey::sub_key(value, list_sort_data->next_sort_data);
//         return base::sub_key(list[list_sort_data->current_index], list_sort_data->next_sort_data);
//     }
// };
//
// template<typename T>
// struct ListSubKey
// {
//     typedef SubKey<void> next;
//
//     typedef T sub_key_type;
//
//     static const T & sub_key(const T & value, void *)
//     {
//         return value;
//     }
// };
//
// template<typename T>
// struct FallbackSubKey<T, typename std::enable_if<has_subscript_operator<T>::value>::type> : ListSubKey<T>
// {
// };

// LNK note that we use the full comparison (not just extracted key) here
template<typename It>
struct FullComparison
{
    typedef typename std::remove_reference<decltype(*std::declval<It>())>::type value_type;
    bool operator()(const value_type & l, const value_type & r) const
    {
        return l < r;
    }
};

// Comparison sort of a radix bucket whose extracted keys are all equal: vergesort over the full comparison, with
// the sorted runs it finds merged by the caller's merge policy (see duckdb_vergesort::inplace_merge_policy), so
// that the merge scratch memory of the nested sort is the one the caller controls
template<typename It, typename ExtractKey, typename Merge>
inline void StdSortFallback(It begin, It end, ExtractKey &, const Merge & merge)
{
    FullComparison<It> comp;
    const auto fallback = [&](const It &fb_begin, const It &fb_end) {
        duckdb_pdqsort::pdqsort_branchless(fb_begin, fb_end, comp);
    };
    duckdb_vergesort::vergesort(begin, end, comp, fallback, merge);
}

template<size_t StdSortThreshold, typename It, typename ExtractKey>
inline bool StdSortIfLessThanThreshold(It begin, It end, size_t num_elements, ExtractKey &)
{
    if (num_elements <= 1)
        return true;
    if (num_elements >= StdSortThreshold)
        return false;
    // below vergesort's minimum size (128), where it would hand the whole range to pdqsort without merging anything
    duckdb_pdqsort::pdqsort_branchless(begin, end, FullComparison<It>());
    return true;
}

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, typename SubKeyType = typename CurrentSubKey::sub_key_type>
struct InplaceSorter;

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, size_t NumBytes, size_t Offset = 0>
struct UnsignedInplaceSorter
{
    static constexpr size_t ShiftAmount = (((NumBytes - 1) - Offset) * 8);
    template<typename T>
    inline static uint8_t current_byte(T && elem, void * sort_data)
    {
        return static_cast<uint8_t>(CurrentSubKey::sub_key(elem, sort_data) >> ShiftAmount);
    }
    template<typename It, typename ExtractKey>
    static void sort(It begin, It end, size_t num_elements, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * sort_data)
    {
    	if (extract_key.Interrupted())
    		return;
    	else if (extract_key.ByteIsSkippable(Offset))
    		UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, NumBytes, Offset + 1>::sort(begin, end, num_elements, extract_key, next_sort, sort_data);
        else if (num_elements < AmericanFlagSortThreshold)
            american_flag_sort(begin, end, extract_key, next_sort, sort_data);
        else
            ska_byte_sort(begin, end, extract_key, next_sort, sort_data);
    }

    template<typename It, typename ExtractKey>
    static void american_flag_sort(It begin, It end, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * sort_data)
    {
        PartitionInfo partitions[256];
        for (It it = begin; it != end; ++it)
        {
            ++partitions[current_byte(extract_key(*it), sort_data)].count;
        }
        size_t total = 0;
        uint8_t remaining_partitions[256];
        int num_partitions = 0;
        for (int i = 0; i < 256; ++i)
        {
            size_t count = partitions[i].count;
            if (!count)
                continue;
            partitions[i].offset = total;
            total += count;
            partitions[i].next_offset = total;
            remaining_partitions[num_partitions] = static_cast<uint8_t>(i);
            ++num_partitions;
        }
        if (num_partitions > 1)
        {
            uint8_t * current_block_ptr = remaining_partitions;
            PartitionInfo * current_block = partitions + *current_block_ptr;
            uint8_t * last_block = remaining_partitions + num_partitions - 1;
            It it = begin;
            It block_end = begin + current_block->next_offset;
            It last_element = end - 1;
            for (;;)
            {
                PartitionInfo * block = partitions + current_byte(extract_key(*it), sort_data);
                if (block == current_block)
                {
                    ++it;
                    if (it == last_element)
                        break;
                    else if (it == block_end)
                    {
                        for (;;)
                        {
                            ++current_block_ptr;
                            if (current_block_ptr == last_block)
                                goto recurse;
                            current_block = partitions + *current_block_ptr;
                            if (current_block->offset != current_block->next_offset)
                                break;
                        }

                        it = begin + current_block->offset;
                        block_end = begin + current_block->next_offset;
                    }
                }
                else
                {
                    size_t offset = block->offset++;
                    std::iter_swap(it, begin + offset);
                }
            }
        }
        recurse:
        if (Offset + 1 != extract_key.ska_sort_width || next_sort)
        {
            size_t start_offset = 0;
            It partition_begin = begin;
            for (uint8_t * it = remaining_partitions, * end_it = remaining_partitions + num_partitions; it != end_it; ++it)
            {
                size_t end_offset = partitions[*it].next_offset;
                It partition_end = begin + end_offset;
                size_t num_elements = end_offset - start_offset;
                if (!StdSortIfLessThanThreshold<StdSortThreshold>(partition_begin, partition_end, num_elements, extract_key))
                {
                    UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, NumBytes, Offset + 1>::sort(partition_begin, partition_end, num_elements, extract_key, next_sort, sort_data);
                }
                start_offset = end_offset;
                partition_begin = partition_end;
            }
        }
    }

    template<typename It, typename ExtractKey>
    static void ska_byte_sort(It begin, It end, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * sort_data)
    {
        PartitionInfo partitions[256];
        for (It it = begin; it != end; ++it)
        {
            ++partitions[current_byte(extract_key(*it), sort_data)].count;
        }
        uint8_t remaining_partitions[256];
        size_t total = 0;
        int num_partitions = 0;
        for (int i = 0; i < 256; ++i)
        {
            size_t count = partitions[i].count;
            if (count)
            {
                partitions[i].offset = total;
                total += count;
                remaining_partitions[num_partitions] = static_cast<uint8_t>(i);
                ++num_partitions;
            }
            partitions[i].next_offset = total;
        }
        for (uint8_t * last_remaining = remaining_partitions + num_partitions, * end_partition = remaining_partitions + 1; last_remaining > end_partition;)
        {
            last_remaining = custom_std_partition(remaining_partitions, last_remaining, [&](uint8_t partition)
            {
                size_t & begin_offset = partitions[partition].offset;
                size_t & end_offset = partitions[partition].next_offset;
                if (begin_offset == end_offset)
                    return false;

                auto partitions_copy = partitions;
                unroll_loop_four_times(begin + begin_offset, end_offset - begin_offset, [&partitions_copy, begin, &extract_key, sort_data](It it)
                {
                    uint8_t this_partition = current_byte(extract_key(*it), sort_data);
                    size_t offset = partitions_copy[this_partition].offset++;
                    std::iter_swap(it, begin + offset);
                });
                return begin_offset != end_offset;
            });
        }
        if (Offset + 1 != extract_key.ska_sort_width || next_sort)
        {
            for (uint8_t * it = remaining_partitions + num_partitions; it != remaining_partitions; --it)
            {
                uint8_t partition = it[-1];
                size_t start_offset = (partition == 0 ? 0 : partitions[partition - 1].next_offset);
                size_t end_offset = partitions[partition].next_offset;
                It partition_begin = begin + start_offset;
                It partition_end = begin + end_offset;
                size_t num_elements = end_offset - start_offset;
                if (!StdSortIfLessThanThreshold<StdSortThreshold>(partition_begin, partition_end, num_elements, extract_key))
                {
                    UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, NumBytes, Offset + 1>::sort(partition_begin, partition_end, num_elements, extract_key, next_sort, sort_data);
                }
            }
        }
    }
};

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, size_t NumBytes>
struct UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, NumBytes, NumBytes>
{
    template<typename It, typename ExtractKey>
    inline static void sort(It begin, It end, size_t num_elements, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * next_sort_data)
    {
        next_sort(begin, end, num_elements, extract_key, next_sort_data);
    }
};

// template<typename It, typename ExtractKey, typename ElementKey>
// size_t CommonPrefix(It begin, It end, size_t start_index, ExtractKey && extract_key, ElementKey && element_key)
// {
//     const auto & largest_match_list = extract_key(*begin);
//     size_t largest_match = largest_match_list.size();
//     if (largest_match == start_index)
//         return start_index;
//     for (++begin; begin != end; ++begin)
//     {
//         const auto & current_list = extract_key(*begin);
//         size_t current_size = current_list.size();
//         if (current_size < largest_match)
//         {
//             largest_match = current_size;
//             if (largest_match == start_index)
//                 return start_index;
//         }
//         if (element_key(largest_match_list[start_index]) != element_key(current_list[start_index]))
//             return start_index;
//         for (size_t i = start_index + 1; i < largest_match; ++i)
//         {
//             if (element_key(largest_match_list[i]) != element_key(current_list[i]))
//             {
//                 largest_match = i;
//                 break;
//             }
//         }
//     }
//     return largest_match;
// }
//
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, typename ListType>
// struct ListInplaceSorter
// {
//     using ElementSubKey = ListElementSubKey<CurrentSubKey, ListType>;
//     template<typename It, typename ExtractKey>
//     static void sort(It begin, It end, ExtractKey & extract_key, ListSortData<It, ExtractKey> * sort_data)
//     {
//         size_t current_index = sort_data->current_index;
//         void * next_sort_data = sort_data->next_sort_data;
//         auto current_key = [&](typename std::remove_reference<decltype(*begin)>::type & elem) -> typename CurrentSubKey::sub_key_type
//         {
//             return CurrentSubKey::sub_key(extract_key(elem), next_sort_data);
//         };
//         auto element_key = [&](typename std::remove_reference<decltype(*begin)>::type & elem) -> typename ElementSubKey::base::sub_key_type
//         {
//             return ElementSubKey::base::sub_key(elem, sort_data);
//         };
//         sort_data->current_index = current_index = CommonPrefix(begin, end, current_index, current_key, element_key);
//         It end_of_shorter_ones = std::partition(begin, end, [&](typename std::remove_reference<decltype(*begin)>::type & elem)
//         {
//             return current_key(elem).size() <= current_index;
//         });
//         size_t num_shorter_ones = end_of_shorter_ones - begin;
//         if (sort_data->next_sort && !StdSortIfLessThanThreshold<StdSortThreshold>(begin, end_of_shorter_ones, num_shorter_ones, extract_key))
//         {
//             sort_data->next_sort(begin, end_of_shorter_ones, num_shorter_ones, extract_key, next_sort_data);
//         }
//         size_t num_elements = end - end_of_shorter_ones;
//         if (!StdSortIfLessThanThreshold<StdSortThreshold>(end_of_shorter_ones, end, num_elements, extract_key))
//         {
//             void (*sort_next_element)(It, It, size_t, ExtractKey &, void *) = static_cast<void (*)(It, It, size_t, ExtractKey &, void *)>(&sort_from_recursion);
//             InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, ElementSubKey>::sort(end_of_shorter_ones, end, num_elements, extract_key, sort_next_element, sort_data);
//         }
//     }
//
//     template<typename It, typename ExtractKey>
//     static void sort_from_recursion(It begin, It end, size_t, ExtractKey & extract_key, void * next_sort_data)
//     {
//         ListSortData<It, ExtractKey> offset = *static_cast<ListSortData<It, ExtractKey> *>(next_sort_data);
//         ++offset.current_index;
//         --offset.recursion_limit;
//         if (offset.recursion_limit == 0)
//         {
//             StdSortFallback(begin, end, extract_key);
//         }
//         else
//         {
//             sort(begin, end, extract_key, &offset);
//         }
//     }
//
//     template<typename It, typename ExtractKey>
//     static void sort(It begin, It end, size_t, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * next_sort_data)
//     {
//         ListSortData<It, ExtractKey> offset;
//         offset.current_index = 0;
//         offset.recursion_limit = 16;
//         offset.next_sort = next_sort;
//         offset.next_sort_data = next_sort_data;
//         sort(begin, end, extract_key, &offset);
//     }
// };

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
struct InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, bool>
{
    template<typename It, typename ExtractKey>
    static void sort(It begin, It end, size_t, ExtractKey & extract_key, void (*next_sort)(It, It, size_t, ExtractKey &, void *), void * sort_data)
    {
        It middle = std::partition(begin, end, [&](typename std::remove_reference<decltype(*begin)>::type & a){ return !CurrentSubKey::sub_key(extract_key(a), sort_data); });
        if (next_sort)
        {
            next_sort(begin, middle, middle - begin, extract_key, sort_data);
            next_sort(middle, end, end - middle, extract_key, sort_data);
        }
    }
};

// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
// struct InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, uint8_t> : UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, 1>
// {
// };
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
// struct InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, uint16_t> : UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, 2>
// {
// };
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
// struct InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, uint32_t> : UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, 4>
// {
// };
template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
struct InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, uint64_t> : UnsignedInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, 8>
{
};
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, typename SubKeyType, typename Enable = void>
// struct FallbackInplaceSorter;
//
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, typename SubKeyType>
// struct InplaceSorter : FallbackInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, SubKeyType>
// {
// };
//
// template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey, typename SubKeyType>
// struct FallbackInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, SubKeyType, typename std::enable_if<has_subscript_operator<SubKeyType>::value>::type>
//     : ListInplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey, SubKeyType>
// {
// };

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
struct SortStarter;
template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold>
struct SortStarter<StdSortThreshold, AmericanFlagSortThreshold, SubKey<void>>
{
    template<typename It, typename ExtractKey>
    static void sort(It, It, size_t, ExtractKey &, void *)
    {
    }
};

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename CurrentSubKey>
struct SortStarter
{
    template<typename It, typename ExtractKey, typename Merge, typename SortNext>
    static void sort(It begin, It end, size_t num_elements, ExtractKey & extract_key, const Merge & merge, const SortNext & sort_next)
    {
        if (StdSortIfLessThanThreshold<StdSortThreshold>(begin, end, num_elements, extract_key))
            return;

        // void (*next_sort)(It, It, size_t, ExtractKey &, void *) = static_cast<void (*)(It, It, size_t, ExtractKey &, void *)>(&SortStarter<StdSortThreshold, AmericanFlagSortThreshold, typename CurrentSubKey::next>::sort);
        // if (next_sort == static_cast<void (*)(It, It, size_t, ExtractKey &, void *)>(&SortStarter<StdSortThreshold, AmericanFlagSortThreshold, SubKey<void>>::sort))
        //     next_sort = nullptr;

        // LNK: different templating for DuckDB
        // The continuation and merge policy outlive all synchronous radix passes.
        struct NextSortData {
            const Merge & merge;
            const SortNext & sort;
        } continuation {merge, sort_next};
        void (*next_sort)(It, It, size_t, ExtractKey &, void *) = nullptr;
        void * next_sort_data = nullptr;
        if (extract_key.requires_next_sort) {
            next_sort = [](It b, It e, size_t, ExtractKey& ek, void* data) {
                const auto & continuation = *static_cast<const NextSortData *>(data);
                continuation.sort(b, e, ek, continuation.merge);
            };
            next_sort_data = &continuation;
        }

        InplaceSorter<StdSortThreshold, AmericanFlagSortThreshold, CurrentSubKey>::sort(begin, end, num_elements, extract_key, next_sort, next_sort_data);
    }
};

template<size_t StdSortThreshold, size_t AmericanFlagSortThreshold, typename It, typename ExtractKey, typename Merge, typename SortNext>
void inplace_radix_sort(It begin, It end, ExtractKey & extract_key, const Merge & merge, const SortNext & sort_next)
{
    typedef SubKey<decltype(extract_key(*begin))> SubKeyType;
    SortStarter<StdSortThreshold, AmericanFlagSortThreshold, SubKeyType>::sort(begin, end, end - begin, extract_key, merge, sort_next);
}

struct IdentityFunctor
{
    template<typename T>
    typename std::remove_reference<T>::type && operator()(T && i) const
    {
        return std::forward<T>(i);
    }
};
}

// Sorts by the extracted radix, then calls sort_next on equal-key buckets that still require sorting.
template<typename It, typename ExtractKey, typename Merge, typename SortNext>
static void ska_sort(It begin, It end, ExtractKey && extract_key, const Merge & merge, const SortNext & sort_next)
{
    detail::inplace_radix_sort<128, 1024>(begin, end, extract_key, merge, sort_next);
}

// By default, equal-key buckets use full comparison and the caller's merge policy.
template<typename It, typename ExtractKey, typename Merge>
static void ska_sort(It begin, It end, ExtractKey && extract_key, const Merge & merge)
{
    const auto sort_next = [](It b, It e, typename std::remove_reference<ExtractKey>::type & ek, const Merge & policy) {
        detail::StdSortFallback(b, e, ek, policy);
    };
    ska_sort(begin, end, extract_key, merge, sort_next);
}

template<typename It, typename ExtractKey>
static void ska_sort(It begin, It end, ExtractKey && extract_key)
{
    ska_sort(begin, end, extract_key, duckdb_vergesort::inplace_merge_policy());
}

template<typename It>
static void ska_sort(It begin, It end)
{
    ska_sort(begin, end, detail::IdentityFunctor());
}

// template<typename It, typename OutIt, typename ExtractKey>
// bool ska_sort_copy(It begin, It end, OutIt buffer_begin, ExtractKey && key)
// {
//     size_t num_elements = end - begin;
//     if (num_elements < 128 || detail::RadixSortPassCount<typename std::result_of<ExtractKey(decltype(*begin))>::type>::value >= 8)
//     {
//         ska_sort(begin, end, key);
//         return false;
//     }
//     else
//         return detail::RadixSorter<typename std::result_of<ExtractKey(decltype(*begin))>::type>::sort(begin, end, buffer_begin, key);
// }
// template<typename It, typename OutIt>
// bool ska_sort_copy(It begin, It end, OutIt buffer_begin)
// {
//     return ska_sort_copy(begin, end, buffer_begin, detail::IdentityFunctor());
// }

} // namespace duckdb_ska_sort
