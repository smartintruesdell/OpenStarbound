#pragma once

#include "StarRect.hpp"
#include "StarVector.hpp"
#include "StarColor.hpp"
#include "StarPoly.hpp"
#include "StarLine.hpp"
#include "StarLua.hpp"
#include "StarVariant.hpp"

namespace Star {

template <typename T>
struct LuaConverter<LuaNullTermWrapper<T>> : LuaConverter<T> {
  static LuaValue from(LuaEngine& engine, LuaNullTermWrapper<T>&& v) {
    auto enforcer = engine.nullTerminate();
    return LuaConverter<T>::from(engine, std::forward<T>(v));
  }

  static LuaValue from(LuaEngine& engine, LuaNullTermWrapper<T> const& v) {
    auto enforcer = engine.nullTerminate();
    return LuaConverter<T>::from(engine, v);
  }

  static LuaNullTermWrapper<T> to(LuaEngine& engine, LuaValue const& v) {
    auto enforcer = engine.nullTerminate();
    return LuaConverter<T>::to(engine, v);
  }
};

template <typename T1, typename T2>
struct LuaConverter<pair<T1, T2>> {
  static LuaValue from(LuaEngine& engine, pair<T1, T2>&& v) {
    auto t = engine.createTable();
    t.set(1, std::move(v.first));
    t.set(2, std::move(v.second));
    return t;
  }

  static LuaValue from(LuaEngine& engine, pair<T1, T2> const& v) {
    auto t = engine.createTable();
    t.set(1, v.first);
    t.set(2, v.second);
    return t;
  }

  static Maybe<pair<T1, T2>> to(LuaEngine& engine, LuaValue const& v) {
    if (auto table = engine.luaMaybeTo<LuaTable>(std::move(v))) {
      auto p1 = engine.luaMaybeTo<T1>(table->get(1));
      auto p2 = engine.luaMaybeTo<T2>(table->get(2));
      if (p1 && p2)
        return {{p1.take(), p2.take()}};
    }
    return {};
  }
};

// Converter for LuaTupleReturn - converts between C++ tuples and Lua tables.
// 
// Edge case behavior:
// - Extra table elements beyond the tuple size are ignored (only indices 1..N are read)
// - Missing required elements cause conversion to fail (returns empty Maybe)
// - Non-sequential indices: only sequential indices starting at 1 are read
// - If a single value is provided (not a table), only the first tuple element is populated,
//   remaining elements are set to LuaNil (which may convert to default values)
template <typename... Types>
struct LuaConverter<LuaTupleReturn<Types...>> {
  template <size_t... Indexes>
  static LuaValue toTable(LuaEngine& engine, LuaTupleReturn<Types...> const& tuple, IndexSequence<Indexes...> const&) {
    auto table = engine.createTable(sizeof...(Types), 0);
    using expander = int[];
    (void)expander{0, (table.set(Indexes + 1, engine.luaFrom(std::get<Indexes>(tuple))), 0)...};
    return table;
  }

  static LuaValue from(LuaEngine& engine, LuaTupleReturn<Types...> const& tuple) {
    return toTable(engine, tuple, typename GenIndexSequence<0, sizeof...(Types)>::type());
  }

  static LuaValue from(LuaEngine& engine, LuaTupleReturn<Types...>&& tuple) {
    return toTable(engine, tuple, typename GenIndexSequence<0, sizeof...(Types)>::type());
  }

  template <size_t... Indexes>
  static Maybe<LuaTupleReturn<Types...>> fromTable(LuaEngine& engine, LuaTable const& table, IndexSequence<Indexes...> const&) {
    try {
      // Only reads sequential indices 1, 2, ..., N. Extra elements are ignored.
      return LuaTupleReturn<Types...>(engine.luaTo<Types>(table.get(Indexes + 1))...);
    } catch (LuaException const&) {
      // Conversion failed - return empty Maybe
      return {};
    }
  }

  template <size_t... Indexes>
  static Maybe<LuaTupleReturn<Types...>> fromSingle(LuaEngine& engine, LuaValue const& v, IndexSequence<Indexes...> const&) {
    try {
      // Single value: first element gets the value, rest get LuaNil
      return LuaTupleReturn<Types...>(engine.luaTo<Types>(Indexes == 0 ? v : LuaNil)...);
    } catch (LuaException const&) {
      return {};
    }
  }

  static Maybe<LuaTupleReturn<Types...>> to(LuaEngine& engine, LuaValue const& v) {
    auto indexes = typename GenIndexSequence<0, sizeof...(Types)>::type();
    if (auto table = v.ptr<LuaTable>())
      return fromTable(engine, *table, indexes);
    if (v == LuaNil)
      return fromSingle(engine, LuaNil, indexes);
    return fromSingle(engine, v, indexes);
  }
};

// Convert LuaVariadic values by treating them as simple Lua arrays.
//
// Edge case behavior:
// - Empty tables convert to empty LuaVariadic (no elements)
// - Non-sequential indices: only sequential indices starting at 1 are read (stops at first gap)
// - Mixed types: if T is a variant type, each element is converted individually
// - Single value: if not a table, converts to 1-element variadic if value is convertible to T
// - Conversion failure: if any element fails to convert, entire conversion fails (returns empty Maybe)
template <typename T>
struct LuaConverter<LuaVariadic<T>> {
  static LuaValue from(LuaEngine& engine, LuaVariadic<T> const& variadic) {
    auto table = engine.createTable(variadic.size(), 0);
    size_t i = 1;
    for (auto const& v : variadic)
      table.set(i++, engine.luaFrom(v));
    return table;
  }

  static Maybe<LuaVariadic<T>> to(LuaEngine& engine, LuaValue const& v) {
    LuaVariadic<T> result;

    // If it's a table, treat sequential numeric keys as an array.
    // Stops at first missing index (non-sequential indices are ignored).
    if (auto table = v.ptr<LuaTable>()) {
      for (size_t i = 1; table->contains(i); ++i) {
        auto converted = engine.luaMaybeTo<T>(table->get(i));
        if (!converted)
          // Any conversion failure causes entire conversion to fail
          return {};
        result.append(converted.take());
      }
      return result;
    }

    // Fallback: a single value convertible to T forms a 1-element variadic.
    if (auto single = engine.luaMaybeTo<T>(v)) {
      result.append(single.take());
      return result;
    }

    return {};
  }
};

template <typename T, size_t N>
struct LuaConverter<Vector<T, N>> {
  static LuaValue from(LuaEngine& engine, Vector<T, N> const& v) {
    return engine.createArrayTable(v);
  }

  static Maybe<Vector<T, N>> to(LuaEngine& engine, LuaValue const& v) {
    auto table = v.ptr<LuaTable>();
    if (!table)
      return {};

    Vector<T, N> vec;
    for (size_t i = 0; i < N; ++i) {
      auto v = engine.luaMaybeTo<T>(table->get(i + 1));
      if (!v)
        return {};
      vec[i] = *v;
    }
    return vec;
  }
};

template <typename T>
struct LuaConverter<Matrix3<T>> {
  static LuaValue from(LuaEngine& engine, Matrix3<T> const& m) {
    auto table = engine.createTable(3, 0);
    table.set(1, m[0]);
    table.set(2, m[1]);
    table.set(3, m[2]);
    return table;
  }

  static Maybe<Matrix3<T>> to(LuaEngine& engine, LuaValue const& v) {
    if (auto table = v.ptr<LuaTable>()) {
      auto r1 = engine.luaMaybeTo<typename Matrix3<T>::Vec3>(table->get(1));
      auto r2 = engine.luaMaybeTo<typename Matrix3<T>::Vec3>(table->get(2));
      auto r3 = engine.luaMaybeTo<typename Matrix3<T>::Vec3>(table->get(3));
      if (r1 && r2 && r3)
        return Matrix3<T>(*r1, *r2, *r3);
    }
    return {};
  }
};

template <typename T>
struct LuaConverter<Rect<T>> {
  static LuaValue from(LuaEngine& engine, Rect<T> const& v) {
    if (v.isNull())
      return LuaNil;
    auto t = engine.createTable();
    t.set(1, v.xMin());
    t.set(2, v.yMin());
    t.set(3, v.xMax());
    t.set(4, v.yMax());
    return t;
  }

  static Maybe<Rect<T>> to(LuaEngine& engine, LuaValue const& v) {
    if (v == LuaNil)
      return Rect<T>::null();

    if (auto table = v.ptr<LuaTable>()) {
      auto xMin = engine.luaMaybeTo<T>(table->get(1));
      auto yMin = engine.luaMaybeTo<T>(table->get(2));
      auto xMax = engine.luaMaybeTo<T>(table->get(3));
      auto yMax = engine.luaMaybeTo<T>(table->get(4));
      if (xMin && yMin && xMax && yMax)
        return Rect<T>(*xMin, *yMin, *xMax, *yMax);
    }
    return {};
  }
};

template <typename T>
struct LuaConverter<Polygon<T>> {
  static LuaValue from(LuaEngine& engine, Polygon<T> const& poly) {
    return engine.createArrayTable(poly.vertexes());
  }

  static Maybe<Polygon<T>> to(LuaEngine& engine, LuaValue const& v) {
    if (auto points = engine.luaMaybeTo<typename Polygon<T>::VertexList>(v))
      return Polygon<T>(points.take());
    return {};
  }
};

template <typename T, size_t N>
struct LuaConverter<Line<T, N>> {
  static LuaValue from(LuaEngine& engine, Line<T, N> const& line) {
    auto table = engine.createTable();
    table.set(1, line.min());
    table.set(2, line.max());
    return table;
  }

  static Maybe<Line<T, N>> to(LuaEngine& engine, LuaValue const& v) {
    if (auto table = v.ptr<LuaTable>()) {
      auto min = engine.luaMaybeTo<Vector<T, N>>(table->get(1));
      auto max = engine.luaMaybeTo<Vector<T, N>>(table->get(2));
      if (min && max)
        return Line<T, N>(*min, *max);
    }
    return {};
  }
};

// Sort of magical converter, tries to convert from all the types in the
// Variant in order, returning the first correct type.  Types should not be
// ambiguous, or the more specific types should come first, which relies on the
// implementation of the converters.
template <typename FirstType, typename... RestTypes>
struct LuaConverter<Variant<FirstType, RestTypes...>> {
  static LuaValue from(LuaEngine& engine, Variant<FirstType, RestTypes...> const& variant) {
    return variant.call([&engine](auto const& a) { return engine.luaFrom(a); });
  }

  static LuaValue from(LuaEngine& engine, Variant<FirstType, RestTypes...>&& variant) {
    return variant.call([&engine](auto& a) { return engine.luaFrom(std::move(a)); });
  }

  static Maybe<Variant<FirstType, RestTypes...>> to(LuaEngine& engine, LuaValue const& v) {
    return checkTypeTo<FirstType, RestTypes...>(engine, v);
  }

  template <typename CheckType1, typename CheckType2, typename... CheckTypeRest>
  static Maybe<Variant<FirstType, RestTypes...>> checkTypeTo(LuaEngine& engine, LuaValue const& v) {
    if (auto t1 = engine.luaMaybeTo<CheckType1>(v))
      return t1;
    else
      return checkTypeTo<CheckType2, CheckTypeRest...>(engine, v);
  }

  template <typename Type>
  static Maybe<Variant<FirstType, RestTypes...>> checkTypeTo(LuaEngine& engine, LuaValue const& v) {
    return engine.luaMaybeTo<Type>(v);
  }

  static Maybe<Variant<FirstType, RestTypes...>> to(LuaEngine& engine, LuaValue&& v) {
    return checkTypeTo<FirstType, RestTypes...>(engine, std::move(v));
  }

  template <typename CheckType1, typename CheckType2, typename... CheckTypeRest>
  static Maybe<Variant<FirstType, RestTypes...>> checkTypeTo(LuaEngine& engine, LuaValue&& v) {
    if (auto t1 = engine.luaMaybeTo<CheckType1>(v))
      return t1;
    else
      return checkTypeTo<CheckType2, CheckTypeRest...>(engine, std::move(v));
  }

  template <typename Type>
  static Maybe<Variant<FirstType, RestTypes...>> checkTypeTo(LuaEngine& engine, LuaValue&& v) {
    return engine.luaMaybeTo<Type>(std::move(v));
  }
};

// Similarly to Variant converter, tries to convert from all types in order.
// An empty MVariant is converted to nil and vice versa.
template <typename... Types>
struct LuaConverter<MVariant<Types...>> {
  static LuaValue from(LuaEngine& engine, MVariant<Types...> const& variant) {
    LuaValue value;
    variant.call([&value, &engine](auto const& a) {
        value = engine.luaFrom(a);
      });
    return value;
  }

  static LuaValue from(LuaEngine& engine, MVariant<Types...>&& variant) {
    LuaValue value;
    variant.call([&value, &engine](auto& a) {
        value = engine.luaFrom(std::move(a));
      });
    return value;
  }

  static Maybe<MVariant<Types...>> to(LuaEngine& engine, LuaValue const& v) {
    if (v == LuaNil)
      return MVariant<Types...>();
    return checkTypeTo<Types...>(engine, v);
  }

  template <typename CheckType1, typename CheckType2, typename... CheckTypeRest>
  static Maybe<MVariant<Types...>> checkTypeTo(LuaEngine& engine, LuaValue const& v) {
    if (auto t1 = engine.luaMaybeTo<CheckType1>(v))
      return t1;
    else
      return checkTypeTo<CheckType2, CheckTypeRest...>(engine, v);
  }

  template <typename CheckType>
  static Maybe<MVariant<Types...>> checkTypeTo(LuaEngine& engine, LuaValue const& v) {
    return engine.luaMaybeTo<CheckType>(v);
  }

  static Maybe<MVariant<Types...>> to(LuaEngine& engine, LuaValue&& v) {
    if (v == LuaNil)
      return MVariant<Types...>();
    return checkTypeTo<Types...>(engine, std::move(v));
  }

  template <typename CheckType1, typename CheckType2, typename... CheckTypeRest>
  static Maybe<MVariant<Types...>> checkTypeTo(LuaEngine& engine, LuaValue&& v) {
    if (auto t1 = engine.luaMaybeTo<CheckType1>(v))
      return t1;
    else
      return checkTypeTo<CheckType2, CheckTypeRest...>(engine, std::move(v));
  }

  template <typename CheckType>
  static Maybe<MVariant<Types...>> checkTypeTo(LuaEngine& engine, LuaValue&& v) {
    return engine.luaMaybeTo<CheckType>(std::move(v));
  }

};

template <>
struct LuaConverter<Color> {
  static LuaValue from(LuaEngine& engine, Color const& c);
  static Maybe<Color> to(LuaEngine& engine, LuaValue const& v);
};

template <>
struct LuaConverter<LuaCallbacks> {
  static LuaValue from(LuaEngine& engine, LuaCallbacks const& c);
  static Maybe<LuaCallbacks> to(LuaEngine& engine, LuaValue const& v);
};

}
