# C++ coding standard

Applies to all C++17. Style: C-style C++ with RAII, no exceptions, no RTTI, no heap allocation per frame. Every rule is mandatory. Rule IDs in brackets refer to the C++ Core Guidelines.

## 1. Naming and layout

| Item | Style |
|---|---|
| Class, struct, enum, alias | `CamelCase` |
| Enumerator | `CamelCase` [Enum.5] |
| Function, method, parameter, variable, member | `camelBack` |
| Constant (`const`, `constexpr`, static member) | `camelBack` [NL.9, ES.9] |
| Macro | `ALL_CAPS`, only macros [NL.9, ES.32] |
| File suffix | `.cpp` code, `.h` interface, `.hpp` header-only [SF.1] |

1. Layout is produced by `.clang-format`: K&R braces, `int* p` declarators [NL.17, NL.18]. One statement per line, one name per declaration [NL.20, NL.21].
2. A name does not encode its type and has at least 3 characters. No easily misread or similar-looking names [NL.5, NL.19, ES.8].
3. Do not reuse a name in a nested scope [ES.12].
4. Class members are declared in this order: types, constructors, assignment, destructor, functions, data. Sections: `public`, `protected`, `private` [NL.16].
5. `const` goes first: `const int x` [NL.26].
6. A literal with more than 4 digits uses digit separators: `1'000'000` [NL.11].
7. A comment states intent, not what the code says, and is short [NL.1, NL.2, NL.3].
8. An unused parameter is unnamed [F.9].
9. Every class, struct, enum and every non-private function declared in a header has a Doxygen block on its declaration, not repeated in the `.cpp` [NL.2]:
   - Style `/** ... */` with `@brief`, `@param` for each parameter, `@return` for a non-void result, `@tparam` for each template parameter.
   - A tag states what the name does not: unit, range, ownership, special values, when the call fails.
   - Defaulted and deleted special members are covered by the class block.
   - A private member and a function that has no header declaration get a short `//` comment, only where the intent is not clear from the name [NL.1, NL.2].

## 2. Files and headers

1. A header has include guards `#ifndef NAME_H`, is self-contained, and contains no object definitions or non-inline function definitions [SF.8, SF.11, SF.2].
2. Declarations used by several `.cpp` files live in a header. A `.cpp` includes the header that defines its interface. Includes come before other declarations [SF.3, SF.4, SF.5].
3. Include what you use. No dependence on names another header happens to include [SF.10].
4. Quoted include for project files relative to the including file, angle brackets for everything else [SF.12].
5. No cyclic dependencies between source files [SF.9].
6. No `using namespace` in a header [SF.7].
7. Logical groups live in a `namespace`. Entities private to a `.cpp` live in an unnamed namespace, never in a header [SF.20, SF.21, SF.22].

## 3. Resources and ownership

1. Every resource (fd, mmap region, DMA-BUF, `FILE*`, RKNN context, RGA handle, MPP handle) has one owner object that releases it in its destructor. No leak on any path [R.1, P.8, C.31].
2. A heap object is owned by `std::unique_ptr`, created with `std::make_unique`. No `std::shared_ptr` [R.20, R.21, R.23].
3. A raw pointer or reference is non-owning. Ownership never moves through `T*` or `T&` [R.3, R.4, I.11].
4. No explicit `new`, `delete`, `malloc`, `free`. Prefer a scoped object to a heap object [R.5, R.10, R.11, ES.60].
5. The result of an allocation goes into its owner in the same statement, one allocation per statement [R.12, R.13].
6. A class that needs an action at destruction defines the destructor. It never fails [C.30, C.36].
7. Define or `=delete` all of copy, move and destructor together, or none of them. A handle class is move-only, and a moved-from handle is valid and releases nothing [C.20, C.21, C.64, C.80, C.81].
8. A copyable or movable type has no `const` or reference data members [C.12].
9. The per-frame path does not allocate or free [Per.14, Per.15].
10. Use `memcpy` and `memset` only on trivially copyable types [SL.con.4].

## 4. Errors

1. No `throw`, `try` or `catch`. Errors are values [E.1, E.25, E.27].
2. A function that can fail returns `bool` or a status type, declared `[[nodiscard]]`. The caller checks every result.
3. A constructor creates a fully initialized, usable object. When construction can fail, a factory function returns `std::optional<T>`, empty on failure. No `init()` or `open()` member called on a constructed object [C.41].
4. Read `errno` immediately after the failing call and convert it to a return value. Never carry `errno` between functions [E.28].
5. An error is logged once, where its context is known (what failed, which object, the cause). Callers return the failure and do not log it again.
6. A violated invariant (a bug, not a runtime failure) logs and calls `std::abort()` [E.26].
7. Destructors, move operations, `swap` and every function on the per-frame path are `noexcept` [C.37, C.66, C.85, F.6, E.12, E.16].
8. Validate input at the program boundary, so errors surface early [P.7].
9. State a function's preconditions and postconditions where the types do not show them [I.5, I.7].

## 5. Interfaces and functions

1. An interface is explicit: all input and output is in parameters and return values [I.1].
2. No non-`const` global variables, no singletons, no non-trivial initialization of global objects [I.2, I.3, I.22, R.6].
3. An interface is precisely and strongly typed: an fd, a size and a flag are not all `int` [I.4].
4. Few parameters. Group related values in a struct. No adjacent parameters of the same type that can be swapped without a compile error [I.23, I.24].
5. A function does one logical operation and has cognitive complexity at most 25. No unnecessary condition nesting [F.1, F.2, F.3, F.56].
6. Input: cheap types by value, others by `const&`. Output: return it, several outputs as a struct. In-out: non-`const` reference. `T*` only when "no argument" is valid [F.15, F.16, F.17, F.20, F.21, F.60].
7. A function that does not take ownership takes `T*` or `T&`, never a smart pointer [F.7, R.30].
8. Never return a pointer or reference to a local object [F.43].
9. A sequence is passed as one object that carries its length: `std::array&`, `std::vector&`, or a `(pointer, size)` pair only inside a wrapper (section 8). Never a single pointer [I.13, R.14].
10. Do not define a C-variadic function. Use a variadic template [F.55].
11. `main` returns `int` [F.46].
12. Use `constexpr` for functions and values computable at compile time. Use `static_assert` for sizes, limits and layout assumptions [F.4, Con.5, P.5].

## 6. Classes

1. Related data goes in one `struct` or `class`. `struct` when members vary independently, `class` when there is an invariant [C.1, C.2].
2. A class with a non-public member is a `class`. All non-`const` data members share one access level. No `protected` data [C.8, C.9, C.133, C.134].
3. A class with an invariant has a constructor that establishes it [C.40, C.41].
4. A single-argument constructor is `explicit` [C.46].
5. Initialize members in declaration order, with default member initializers for constant values, and by initialization rather than assignment [C.47, C.48, C.49].
6. A member function is `const` unless it changes the object [Con.2].
7. A function is a member only when it needs access to the representation [C.4].
8. No trivial getters and setters [C.131].
9. No virtual calls in a constructor or destructor [C.82].
10. A base class destructor is public and virtual, or protected and non-virtual. A polymorphic class has no public copy or move [C.35, C.67].
11. Prefer concrete types to class hierarchies. No `dynamic_cast` and no `typeid` [C.10].
12. An enumeration is an `enum class`, not a macro and not an unnamed `enum`. Specify values and underlying type only when needed [Enum.1, Enum.2, Enum.3, Enum.6, Enum.7, Enum.8].

## 7. Expressions and statements

1. Initialize every object at declaration. Declare it at first use, in the smallest scope [ES.20, ES.21, ES.22, ES.5, ES.6]. Initialize with `{}`: a value or a list of elements. A container size or an iterator range is given with `(...)` [ES.23].
2. Declare an object `const` or `constexpr` unless it is modified later. Objects, member functions, pointers and references are immutable by default [ES.25, Con.1, Con.3, Con.4].
3. One variable has one purpose [ES.26].
4. A constant is a named value that states its meaning, not its number and not the place that uses it. No magic constants [ES.45].

   ```cpp
   constexpr int argsDump{5};       // bad: the name gives no meaning
   const bool dump{argc == 5};      // bad: an argument count selects a feature
   ```

5. `argc` and `argv` are read only inside the function that parses the command line into a struct. The rest of the program reads the struct's fields. No feature is inferred from `argc` outside it.

   ```cpp
   struct Options {
       const char* modelPath{nullptr};
       int loops{0};
       const char* inputPath{nullptr};  // nullptr: fixed random input
       const char* dumpPath{nullptr};   // nullptr: no dump
   };
   ```

6. No lossy conversion. One expression does not mix signed and unsigned operands: convert first, with a range check. Sizes and indexes are `std::size_t`, bit manipulation uses unsigned types, all other arithmetic uses signed types. No overflow, underflow or division by zero [ES.46, ES.100, ES.101, ES.102, ES.103, ES.104, ES.105].
7. Avoid casts. A needed cast is a named cast: `static_cast` anywhere, `reinterpret_cast` only inside a wrapper (section 8). Never a C-style or functional cast. Never cast away `const` [ES.48, ES.49, ES.50].
8. `nullptr`, never `0` or `NULL` [ES.47].
9. Parenthesize when in doubt about precedence. No expression depends on the order of evaluation [ES.40, ES.41, ES.43, ES.44].
10. Use `std::array` or `std::vector`, not a C array [ES.27, SL.con.1, SL.con.2, SL.con.3].
11. Prefer a range-`for` where it fits, a `for` when there is a loop variable, `switch` when there is a choice. A loop control variable is not modified in the body [ES.71, ES.72, ES.73, ES.70, ES.86].
12. No `do`-statement, no `goto`. A `switch` has no implicit fallthrough and a `default` for common cases only [ES.75, ES.76, ES.78, ES.79].
13. Do not slice. Do not dereference an invalid pointer. Do not compare pointers into different arrays [ES.63, ES.65, ES.62].
14. Make empty statements visible [ES.85].
15. No macros for constants, functions or program text. Macros are used only for include guards and conditional compilation, with unique `ALL_CAPS` names [ES.30, ES.31, ES.32, ES.33].
16. Use the standard library, and use it type-safely [SL.1, SL.2, SL.4, ES.1].
17. Own character sequences with `std::string` and refer to them with `std::string_view`. A `const char*` refers to a zero-terminated C string [SL.str.1, SL.str.2, SL.str.3].

## 8. C APIs

1. Code that calls an OS or vendor API (ioctl and V4L2, mmap, DMA-heap, RKNN, RGA, MPP, the `argv` of `main`) lives in a wrapper class or function for that API, in its own files. The wrapper converts between C types and project types. Unions, `reinterpret_cast`, pointer arithmetic and `(pointer, size)` pairs never appear in the code that uses the wrapper [I.30, P.11].
2. No signal handler functions. Signals are blocked with `sigprocmask` and read through `signalfd` in the event loop.
3. `volatile` is used only for memory outside C++, never for synchronization [CP.8, CP.200].

## 9. Concurrency

1. Write every function as if it runs in a multi-threaded program. No data races. Share as little writable data as possible [CP.1, CP.2, CP.3].
2. A thread is joined in a scope that owns it. Never `detach()` [CP.23, CP.26].
3. A lock is held through RAII, never plain `lock()` and `unlock()`. Acquire several mutexes with `std::scoped_lock`. A lock guard has a name [CP.20, CP.21, CP.44].
4. Define a mutex together with the data it guards. Keep the critical section short. Never call unknown code (a callback) while holding a lock [CP.50, CP.43, CP.22].
5. Pass small amounts of data between threads by value [CP.31].
6. Never wait without a condition [CP.42].
7. No lock-free programming. Shared data is guarded by a mutex [CP.100].
