#include "CompatGameImpl.h"

#include "GameImpl.h"
#include "UnitImpl.h"
#include "PlayerImpl.h"
#include "BulletImpl.h"

#include <string.h>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <type_traits>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#else
//#include <dlfcn.h>
#include <sys/mman.h>
#endif

namespace {

void* (*msvcr120_new)(size_t);
void (*msvcr120_delete)(void*);


struct std_string {
  char m_buf[0x10] = {0};
  size_t m_size = 0;
  size_t m_allocated = 15;
  
  void* ptr() {
    return m_allocated >= 0x10 ? *(char**)m_buf : m_buf;
  }
  
  std_string() = default;
  std_string(const std::string& value) {
    assign(value.data(), value.size());
  }
  
  ~std_string() {
    if (m_allocated >= 0x10) {
      msvcr120_delete(ptr());
    }
  }
  
  void assign(const char* str, size_t len) {
    if (m_allocated >= 0x10) {
      msvcr120_delete(ptr());
    }
    if (len < 0x10) {
      memcpy(m_buf, str, len);
      m_buf[len] = 0;
      m_size = len;
      m_allocated = 0xf;
    } else {
      char* newbuf = (char*)msvcr120_new(len + 1);
      memcpy(newbuf, str, len);
      newbuf[len] = 0;
      *(char**)m_buf = newbuf;
      m_size = len;
      m_allocated = len + 1;
    }
    
  }
  
};

template<typename T>
struct std_deque {
  void** proxy = nullptr;
  T** m_map = nullptr;
  size_t m_map_size = 0;
  size_t m_offset = 0;
  size_t m_size = 0;
  
  const size_t n_per = 0x10 / sizeof(T);
  
  std_deque() {
    proxy = (void**)msvcr120_new(2 * sizeof(void*));
    proxy[0] = this;
  }
  
   ~std_deque() {
    if (m_map) {
      for (size_t i = 0; i != m_map_size; ++i) {
        if (m_map[i]) {
          for (size_t i2 = 0; i2 != n_per; ++i2) {
            m_map[i][i2].~T();
          }
          msvcr120_delete(m_map[i]);
        }
      }
      msvcr120_delete(m_map);
    }
    msvcr120_delete(proxy);
  }
  
  bool empty() {
    return m_size == 0;
  }
  
  void push_back(T v) {
    size_t newsize = m_size + 1;
    if (m_map_size <= (newsize + n_per - 1) / n_per) {
      size_t new_map_size = std::max((size_t)8, m_map_size * 2);
      T** newmap = (T**)msvcr120_new(new_map_size * sizeof(T**));
      for (size_t i = 0; i != m_map_size; ++i) newmap[i] = m_map[i];
      for (size_t i = m_map_size; i != new_map_size; ++i) newmap[i] = nullptr;
      if (m_map) msvcr120_delete(m_map);
      m_map = newmap;
      m_map_size = new_map_size;
    }
    T*& arr = m_map[m_size / n_per];
    if (!arr) {
      arr = (T*)msvcr120_new(0x10);
    }
    arr[m_size % n_per] = v;
    m_size = newsize;
  }
};

template<typename T>
struct std_list {
  struct node {
    node* next = nullptr;
    node* prev = nullptr;
    T value;
  };
  node* m_head = nullptr;
  size_t m_size = 0;
  
  std_list() {
    m_head = (node*)msvcr120_new(sizeof(node));
    m_head->next = m_head;
    m_head->prev = m_head;
  }
  
  void clear() {
    auto* begin = m_head->next;
    auto* end = m_head;
    for (auto i = begin; i != end; ) {
      auto next = i->next;
      msvcr120_delete(i);
      i = next;
    }
    m_head->next = m_head;
    m_head->prev = m_head;
    m_size = 0;
  }
  
  node* insert(T value, node* before) {
    node* n = (node*)msvcr120_new(sizeof(node));
    new (&n->value) T(value);
    n->next = before;
    n->prev = before->prev;
    before->prev->next = n;
    before->prev = n;
    ++m_size;
    return n;
  }
  
  void push_back(T value) {
    insert(value, m_head);
  }
  T& back() {
    return m_head->prev->value;
  }
  
  node* begin() {
    return m_head->next;
  }
  node* end() {
    return m_head;
  }
  
  size_t size() {
    return m_size;
  }
  
};

template<typename T>
struct std_vector {
  T* m_begin = nullptr;
  T* m_end = nullptr;
  T* m_cap_end = nullptr;
  
  size_t size() {
    return m_end - m_begin;
  }
  
  size_t capacity() {
    return m_cap_end - m_begin;
  }
  
  void push_back(T val) {
    if (capacity() - size() < 1) {
      size_t oldsize = size();
      size_t newcap = std::max((size_t)0x10, capacity() + capacity() / 2);
      T* newarr = (T*)msvcr120_new(newcap * sizeof(T));
      if (m_begin) {
        memcpy(newarr, m_begin, oldsize * sizeof(T));
        msvcr120_delete(m_begin);
      }
      m_begin = newarr;
      m_end = m_begin + oldsize;
      m_cap_end = m_begin + newcap;
    }
    *m_end = val;
    ++m_end;
  }
  
  T* begin() {
    return m_begin;
  }
  T* end() {
    return m_end;
  }
  
  T& operator[](size_t index) {
    return m_begin[index];
  }
  
};

template<typename T>
struct std_unordered_set {
  std_list<T> list;
  std_vector<typename std_list<T>::node*> buckets;
  size_t mask = 7;
  size_t max_index = 8;
  float max_bucket_size = 1.0f;
  
  std_unordered_set() {
    for (size_t i = 0; i != 0x10; ++i) {
      buckets.push_back(list.m_head);
    }
  }
  
  size_t hash(void* ptr) {
    unsigned char* c = (unsigned char*)&ptr;
    size_t r = 2166136261;
    r ^= c[0];
    r *= 16777619;
    r ^= c[1];
    r *= 16777619;
    r ^= c[2];
    r *= 16777619;
    r ^= c[3];
    r *= 16777619;
    return r;
  }
  
  void clear() {
    list.clear();
    for (auto& v : buckets) v = list.m_head;
  }
  
  void insert(T value) {
    size_t index = hash(value) & mask;
    if (index >= max_index) index -= mask / 2 + 1;
    
    auto* begin = buckets[2 * index];
    auto* end = begin == list.end() ? begin : buckets[2 * index + 1];
    for (auto i = begin; i != end; i = i->next) {
      if (i->value == value) return;
    }
    auto* n = list.insert(value, end);
    if (begin == list.end()) {
      buckets[2 * index] = n;
      buckets[2 * index + 1] = n->next;
    } else if (begin == end) {
      buckets[2 * index] = n;
    } else {
      auto* x = buckets[2 * index + 1]->prev;
      buckets[2 * index + 1] = x;
      if (x != list.begin()) buckets[2 * index + 1] = buckets[2 * index + 1]->prev;
    }
    
    
  }
  
};

}

namespace {

template<typename T, typename std::enable_if<std::is_same<T, void>::value>::type* = nullptr>
T* fix_pointer_impl(T* ptr) {
  return ptr;
}

BWAPI::ForceInterface* fix_pointer_impl(BWAPI::ForceInterface* ptr) {
  throw std::runtime_error("ForceInterface");
  return nullptr;
}

BWAPI::RegionInterface* fix_pointer_impl(BWAPI::RegionInterface* ptr) {
  throw std::runtime_error("RegionInterface");
  return nullptr;
}

BWAPI::PlayerInterface* fix_pointer_impl(BWAPI::PlayerInterface* ptr) {
  return (BWAPI::PlayerInterface*)&((BWAPI::PlayerImpl*)ptr)->compatPlayerImpl;
}

BWAPI::UnitInterface* fix_pointer_impl(BWAPI::UnitInterface* ptr) {
  return (BWAPI::UnitInterface*)&((BWAPI::UnitImpl*)ptr)->compatUnitImpl;
}

BWAPI::BulletInterface* fix_pointer_impl(BWAPI::BulletInterface* ptr) {
  return (BWAPI::BulletInterface*)&((BWAPI::BulletImpl*)ptr)->compatBulletImpl;
}

template<typename T>
T* fix_pointer(T* ptr) {
  return ptr ? fix_pointer_impl(ptr) : nullptr;
}

template<typename T, typename std::enable_if<std::is_fundamental<T>::value>::type* = nullptr>
T* unfix_pointer_impl(T* ptr) {
  return ptr;
}

BWAPI::PlayerInterface* unfix_pointer_impl(BWAPI::PlayerInterface* ptr) {
  return ((BWAPI::CompatPlayerImpl*)ptr)->impl;
}
BWAPI::UnitInterface* unfix_pointer_impl(BWAPI::UnitInterface* ptr) {
  return ((BWAPI::CompatUnitImpl*)ptr)->impl;
}

template<typename T>
T* unfix_pointer(T* ptr) {
  return ptr ? unfix_pointer_impl(ptr) : nullptr;
}

}

namespace compat {

enum class memory_access {
      none,
      read,
      read_write,
      read_execute,
      read_write_execute
};

#ifdef _WIN32
void* allocate(size_t size, memory_access access) {
  DWORD protect = 0;
  if (access == memory_access::none) protect = PAGE_NOACCESS;
  else if (access == memory_access::read) protect = PAGE_READONLY;
  else if (access == memory_access::read_write) protect = PAGE_READWRITE;
  else if (access == memory_access::read_execute) protect = PAGE_EXECUTE_READ;
  else if (access == memory_access::read_write_execute) protect = PAGE_EXECUTE_READWRITE;
  return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, protect);
}
#else
void* allocate(size_t size, memory_access access) {
  int protect = 0;
  if (access == memory_access::none) protect = PROT_NONE;
  else if (access == memory_access::read) protect = PROT_READ;
  else if (access == memory_access::read_write) protect = PROT_READ | PROT_WRITE;
  else if (access == memory_access::read_execute) protect = PROT_READ | PROT_EXEC;
  else if (access == memory_access::read_write_execute) protect = PROT_READ | PROT_WRITE | PROT_EXEC;
  return mmap(nullptr, size, protect, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
#endif

void unimplemented_stub(const char* name, void* retaddr) {
  printf("unimplemented: %s (called from %p)", name, retaddr);
  throw std::runtime_error("unimplemented");
}

uint8_t* codegen_pos = nullptr;
uint8_t* codegen_end = nullptr;

uint8_t*& align(uint8_t*& p, int n) {
  while ((uintptr_t)p % 4) ++p;
  return p;
}

void codegen_reserve(size_t n) {
  if (codegen_pos + n > codegen_end) {
    void* mem = allocate(0x10000, memory_access::read_write_execute);
    if (!mem) throw std::runtime_error("memory allocation failed");
    memset(mem, 0xcc, 0x10000);
    codegen_pos = (uint8_t*)mem;
    codegen_end = codegen_pos + 0x10000;
  }
}

void* generate_unimplemented_stub(std::string name) {
  if (name.size() >= 0x400) name.resize(0x400);
  codegen_reserve(name.size() + 1 + 0x10);

  uint8_t* p = codegen_pos;
  char* name_ptr = (char*)p;
  memcpy(name_ptr, name.c_str(), name.size() + 1);
  p += name.size() + 1;

  p = (uint8_t*)(((uintptr_t)p + 3) & -4);
  auto* r = p;

  *p++ = 0x68; // push name_ptr
  *(uint32_t*)p = (uint32_t)name_ptr;
  p += 4;
  *p++ = 0xe8; // call unimplemented_stub
  *(uint32_t*)p = (uint32_t)&unimplemented_stub - (uint32_t)p - 4;
  p += 4;
  *p++ = 0xcc; // breakpoint

  codegen_pos = p;

  return r;
}

void* (*f_call_thiscall)(void* f, void* this_);
void* (*f_call_thiscall_1)(void* f, void* this_, uint32_t);
void* (*f_call_thiscall_2)(void* f, void* this_, uint32_t, uint32_t);

void* call_thiscall(void* f, void* this_) {
  return f_call_thiscall(f, this_);
}

void* call_thiscall(void* f, void* this_, bool a) {
  return f_call_thiscall_1(f, this_, a);
}
void* call_thiscall(void* f, void* this_, void* a) {
  return f_call_thiscall_1(f, this_, (uint32_t)a);
}
void* call_thiscall(void* f, void* this_, BWAPI::Position a) {
  return f_call_thiscall_2(f, this_, a.x, a.y);
}

void generate_functions() {
  codegen_reserve(0x1000);
  uint8_t* p = codegen_pos;
  (void*&)f_call_thiscall = align(p, 4);
  memcpy(p, "\x8b\x54\x24\x04\x8b\x4c\x24\x08\xff\xe2", 0xa);
  p += 0x14;
  (void*&)f_call_thiscall_1 = align(p, 4);
  memcpy(p, "\x8b\x54\x24\x04\x8b\x4c\x24\x08\xff\x74\x24\x0c\xff\xd2\xc3", 0xf);
  p += 0x17;
  (void*&)f_call_thiscall_2 = align(p, 4);
  memcpy(p, "\x8b\x54\x24\x04\x8b\x4c\x24\x08\xff\x74\x24\x0c\xff\x74\x24\x10\xff\xe2\xc3", 0x13);
  p += 0x1b;
  codegen_pos = p;
}

template<typename T>
struct reference_to_pointer {
  using type = typename std::conditional<std::is_reference<T>::value, typename std::remove_reference<T>::type*, T>::type;
};

template<typename... A>
struct argsize;

template<>
struct argsize<>: std::integral_constant<size_t, 0> {};
template<typename T, typename... A>
struct argsize<T, A...>: std::integral_constant<size_t, (sizeof(typename reference_to_pointer<T>::type) + 3) / 4 + argsize<A...>::value> {};

template<size_t I, typename... A>
struct construct_args;

template<size_t I>
struct construct_args<I> {
  template<typename Tuple>
  void operator()(Tuple& t, uint32_t*& args) {
  }
};


template<typename T>
struct fix_argument_type {
  using type = T;
};

template<>
struct fix_argument_type<const BWAPI::Unitset&> {
  using type = BWAPI::Unitset;
};

template<>
struct fix_argument_type<const BWAPI::UnitFilter&> {
  using type = BWAPI::UnitFilter;
};

template<>
struct fix_argument_type<const BWAPI::BestUnitFilter&> {
  using type = BWAPI::BestUnitFilter;
};

template<typename A, int B>
std::true_type is_bwapi_type_func(const BWAPI::Type<A, B>*);
std::false_type is_bwapi_type_func(...);

template<typename T>
using is_bwapi_type = decltype(is_bwapi_type_func(std::declval<typename std::remove_reference<T>::type*>()));

template<typename A, int B>
std::true_type is_bwapi_point_func(const BWAPI::Point<A, B>*);
std::false_type is_bwapi_point_func(...);

template<typename T>
using is_bwapi_point = decltype(is_bwapi_point_func(std::declval<typename std::remove_reference<T>::type*>()));

template<typename T>
typename fix_argument_type<T>::type construct_arg(uint32_t*& args) {
  if (std::is_trivial<T>::value && sizeof(T) <= 4) {
    return (typename reference_to_pointer<T>::type&)*args++;
  }
  if (is_bwapi_type<T>::value || is_bwapi_point<T>::value) {
    auto* ptr = args;
    args += (sizeof(T) + 3) / 4;
    return *(typename reference_to_pointer<T>::type*)ptr;
  }
  printf("unhandled argument type %s\n", typeid(T).name());
  throw std::runtime_error("unhandled argument");
  return {};
}

template<>
BWAPI::UnitCommand construct_arg<BWAPI::UnitCommand>(uint32_t*& args) {
  BWAPI::UnitCommand r = *(BWAPI::UnitCommand*)args;
  args += (sizeof(BWAPI::UnitCommand) + 3) / 4;
  r.unit = unfix_pointer(r.unit);
  r.target = unfix_pointer(r.target);
  return r;
}

template<>
BWAPI::Unitset construct_arg<const BWAPI::Unitset&>(uint32_t*& args) {
  throw std::runtime_error("unitset");
}

template<>
BWAPI::UnitFilter construct_arg<const BWAPI::UnitFilter&>(uint32_t*& args) {
  void* f = ((void**)*args++)[4];
  if (!f) return nullptr;
  return [f](BWAPI::UnitInterface* u) {
    void** vftable = *(void***)f;
    BWAPI::UnitInterface* new_u = fix_pointer(u);
    return (bool)call_thiscall(vftable[2], f, &new_u);
  };
}

template<>
BWAPI::BestUnitFilter construct_arg<const BWAPI::BestUnitFilter&>(uint32_t*& args) {
  throw std::runtime_error("BestUnitFilter");
}


template<size_t I, typename T, typename... A>
struct construct_args<I, T, A...> {
  template<typename Tuple>
  void operator()(Tuple& t, uint32_t*& args) {
    std::get<I>(t).construct(construct_arg<T>(args));
    construct_args<I + 1, A...>()(t, args);
  }
};

template<size_t I, typename T, typename... A>
struct construct_args<I, T*, A...> {
  template<typename Tuple>
  void operator()(Tuple& t, uint32_t*& args) {
    std::get<I>(t).construct(unfix_pointer((T*)*args++));
    construct_args<I + 1, A...>()(t, args);
  }
};


void printt() {
}

template<typename T, typename std::enable_if<is_bwapi_type<T>::value>::type* = nullptr>
void convert_result(T* result, T value) {
  *result = value;
}
void convert_result(BWAPI::Position* result, BWAPI::Position value) {
  *result = value;
}
void convert_result(BWAPI::TilePosition* result, BWAPI::TilePosition value) {
  *result = value;
}
void convert_result(BWAPI::Error* result, BWAPI::Error value) {
  *result = value;
}
void convert_result(std::string* result, std::string value) {
  new (result) std_string(value);
}
void convert_result(BWAPI::UnitCommand* result, const BWAPI::UnitCommand& value) {
  result->unit = fix_pointer(value.unit);
  result->type = value.type;
  result->target = fix_pointer(value.target);
  result->x = value.x;
  result->y = value.y;
  result->extra = value.extra;
}
void convert_result(BWAPI::UnitType::list* result, const BWAPI::UnitType::list& value) {
  auto* ptr = (std_deque<BWAPI::UnitType>*)result;
  new (ptr) std_deque<BWAPI::UnitType>();
  for (auto& v : value) ptr->push_back(v);
}
void convert_result(BWAPI::Unitset* result, const BWAPI::Unitset& value) {
  auto* ptr = (std_unordered_set<BWAPI::UnitInterface*>*)result;
  new (ptr) std_unordered_set<BWAPI::UnitInterface*>();
  for (auto& v : value) {
    ptr->insert(fix_pointer(v));
  }
}

template<typename T>
struct returned_through_pointer_arg : std::integral_constant<bool, !std::is_same<T, void>::value && !std::is_pod<T>::value && !std::is_reference<T>::value> {};

template<typename F, typename M, typename T, size_t... I>
auto apply_impl(F f, M* m, T&& t, std::index_sequence<I...>) {
  return (m->*f)(*std::get<I>(t)...);
}

template<typename F, typename M, typename T>
auto apply(F f, M* m, T&& t) {
  return apply_impl(f, m, std::forward<T>(t), std::make_index_sequence<std::tuple_size<typename std::decay<T>::type>::value>{});
}

template<typename R, typename M, typename T, typename... A>
auto apply(R(M::*f)(A...), M* m, T&& t) {
  return apply_impl(f, m, std::forward<T>(t), std::make_index_sequence<std::tuple_size<typename std::decay<T>::type>::value>{});
}

template<typename R, typename M, typename T, typename... A>
auto apply(R*(M::*f)(A...), M* m, T&& t) {
  return fix_pointer(apply_impl(f, m, std::forward<T>(t), std::make_index_sequence<std::tuple_size<typename std::decay<T>::type>::value>{}));
}

template<typename R, typename M, typename T, typename... A, typename std::enable_if<!returned_through_pointer_arg<R>::value>::type* = nullptr>
auto apply(R(M::*f)(A...), M* m, T&& t, typename std::remove_reference<R>::type* result) {
  return apply(f, m, t);
}

template<typename R, typename M, typename T, typename... A, typename std::enable_if<returned_through_pointer_arg<R>::value>::type* = nullptr>
auto apply(R(M::*f)(A...), M* m, T&& t, R* result) {
  auto r = apply_impl(f, m, std::forward<T>(t), std::make_index_sequence<std::tuple_size<typename std::decay<T>::type>::value>{});
  convert_result(result, std::move(r));
  return result;
}

template<typename F>
struct unwrap;

template<>
struct unwrap<BWAPI::CompatGameImpl> {
  template<typename T>
  T* operator()(T* src) {
    return (T*)((BWAPI::CompatGameImpl*)src)->gameImpl;
  }
};

template<>
struct unwrap<BWAPI::CompatUnitImpl> {
  template<typename T>
  T* operator()(T* src) {
    return (T*)((BWAPI::CompatUnitImpl*)src)->impl;
  }
};

template<>
struct unwrap<BWAPI::CompatPlayerImpl> {
  template<typename T>
  T* operator()(T* src) {
    return (T*)((BWAPI::CompatPlayerImpl*)src)->impl;
  }
};

template<>
struct unwrap<BWAPI::CompatBulletImpl> {
  template<typename T>
  T* operator()(T* src) {
    return (T*)((BWAPI::CompatBulletImpl*)src)->impl;
  }
};

template<typename T, typename E = void>
struct uninitialized;

template<typename T>
struct uninitialized<T, typename std::enable_if<!std::is_reference<T>::value>::type> {
  typename std::aligned_storage<sizeof(T), alignof(T)>::type buf;
  ~uninitialized() {
    ((T&)buf).~T();
  }
  template<typename... A>
  void construct(A&&... args) {
    new ((T*)&buf) T(std::forward<A>(args)...);
  }
  T& operator*() {
    return (T&)buf;
  }
};
template<typename T>
struct uninitialized<T, typename std::enable_if<std::is_reference<T>::value>::type> {
  typename std::remove_reference<T>::type* ptr;
  void construct(T& v) {
    ptr = &v;
  }
  T& operator*() {
    return *ptr;
  }
};

template<typename W, typename R, typename M, typename... A>
auto do_thiscall_func(R(M::**f)(A...), M* m, uint32_t* args, const char* name) {
  size_t n = argsize<A...>::value;
  
  std::tuple<uninitialized<typename fix_argument_type<A>::type>...> c;
  
  auto* result = (typename std::remove_reference<R>::type*)*args;
  if (returned_through_pointer_arg<R>::value) ++args;
  
  construct_args<0, A...>()(c, args);
  
  m = unwrap<W>()(m);
  
  return apply(*f, m, c, result);
}

template<typename W, typename R, typename M, typename... A>
void* generate_thiscall_func(R(M::*f)(A...), const char* name) {
  void* func = (void*)&do_thiscall_func<W, R, M, A...>;
  
  codegen_reserve(0x1000);
  uint8_t* p = codegen_pos;
  align(p, 8);
  uint8_t* ptr_ptr = p;
  memcpy(ptr_ptr, &f, sizeof(f));
  p += sizeof(f);
  uint8_t* name_ptr = p;
  size_t namelen = std::min(strlen(name), (size_t)0x100);
  memcpy(name_ptr, name, namelen);
  p += namelen;
  *p++ = 0;
  
  size_t n = argsize<A...>::value;
  
  if (returned_through_pointer_arg<R>::value) {
    ++n;
  }
  
  align(p, 16);
  void* retval = p;
  
  *p++ = 0x68; // push name_ptr
  *(uint32_t*)p = (uint32_t)name_ptr;
  p += 4;
  *p++ = 0x8d; *p++ = 0x44; *p++ = 0x24; *p++ = 0x08; // lea eax, [esp + 8]
  *p++ = 0x50; // push eax
  *p++ = 0x51; // push ecx
  *p++ = 0x68; // push ptr_ptr
  *(uint32_t*)p = (uint32_t)ptr_ptr;
  p += 4;
  *p++ = 0xe8; // call func
  *(uint32_t*)p = (uint32_t)func - (uint32_t)p - 4;
  p += 4;
  *p++ = 0x83; *p++ = 0xc4; *p++ = 0x10; // add esp, 16
  *p++ = 0xc2; *p++ = n * 4; *p++ = (n * 4) >> 8; // ret n * 4
  
  codegen_pos = p;
  
  return retval;
}

template<typename W, typename R, typename M, typename... A>
void* generate_thiscall_func(R(M::*f)(A...) const, const char* name) {
  return generate_thiscall_func<W>((R(M::*)(A...))f, name);
}

template<typename W, typename T>
void* wrap_thiscall(T f, const char* name) {
  return generate_thiscall_func<W>(f, name);
}


}

namespace BWAPI {

namespace {

struct GameImplImpl {
  std_deque<TilePosition> startLocations;
  
  template<typename T>
  struct proxy {
    int lastUpdate = 0;
    T value;
  };
  
  using Unitset = std_unordered_set<UnitInterface*>;
  using Playerset = std_unordered_set<PlayerInterface*>;
  using Bulletset = std_unordered_set<BulletInterface*>;
  
  proxy<Playerset> players;
  
  proxy<Unitset> allUnits;
  proxy<Unitset> minerals;
  proxy<Unitset> geysers;
  proxy<Unitset> neutralUnits;
  
  proxy<Unitset> staticMinerals;
  proxy<Unitset> staticGeysers;
  proxy<Unitset> staticNeutralUnits;
  
  proxy<Bulletset> bullets;
  
  proxy<Playerset> allies;
  proxy<Playerset> enemies;
  proxy<Playerset> observers;
  
  std::array<proxy<Unitset>, 12> playerUnits;
};

std::unique_ptr<GameImplImpl> gameImplImpl;

CompatGameImpl* compatGameImpl;

template<typename T, typename V>
auto retset(T& proxy, const V& values) {
  if (proxy.lastUpdate != compatGameImpl->gameImpl->serverUpdateCount) {
    proxy.lastUpdate = compatGameImpl->gameImpl->serverUpdateCount;
    proxy.value.clear();
    for (auto* v : values) {
      proxy.value.insert(fix_pointer(v));
    }
  }
  return &proxy.value;
}

struct GameImpl_funcs {
  
  template<typename F, typename... A>
  auto invoke(F f, A&&... args) {
    return compat::apply(f, (GameImpl*)this, std::forward_as_tuple(std::forward<A>(args)...));
  }
  
  GameImpl* gameImpl() {
    return (GameImpl*)this;
  }
  
  void* getStartLocations() {
    if (gameImplImpl->startLocations.empty()) {
      auto& locs = gameImpl()->getStartLocations();
      for (auto& v : locs) {
        gameImplImpl->startLocations.push_back(v);
      }
    }
    return &gameImplImpl->startLocations;
  }
  
  void getForces() {
    throw std::runtime_error("getForces");
  }
  void* getPlayers() {
    return retset(gameImplImpl->players, gameImpl()->getPlayers());
  }
  void* getAllUnits() {
    return retset(gameImplImpl->allUnits, gameImpl()->getAllUnits());
  }
  void* getMinerals() {
    return retset(gameImplImpl->minerals, gameImpl()->getMinerals());
  }
  void* getGeysers() {
    return retset(gameImplImpl->geysers, gameImpl()->getGeysers());
  }
  void* getNeutralUnits() {
    return retset(gameImplImpl->neutralUnits, gameImpl()->getNeutralUnits());
  }
  void* getStaticMinerals() {
    return retset(gameImplImpl->staticMinerals, gameImpl()->getStaticMinerals());
  }
  void* getStaticGeysers() {
    return retset(gameImplImpl->staticGeysers, gameImpl()->getStaticGeysers());
  }
  void* getStaticNeutralUnits() {
    return retset(gameImplImpl->staticNeutralUnits, gameImpl()->getStaticNeutralUnits());
  }
  void* getBullets() {
    return retset(gameImplImpl->bullets, gameImpl()->getBullets());
  }
  void getNukeDots() {
    throw std::runtime_error("getNukeDots");
  }
  void getEvents() {
    throw std::runtime_error("getEvents");
  }

  void* allies() {
    return retset(gameImplImpl->allies, gameImpl()->allies());
  }
  void* enemies() {
    return retset(gameImplImpl->enemies, gameImpl()->enemies());
  }
  void* observers() {
    return retset(gameImplImpl->observers, gameImpl()->observers());
  }
};

struct UnitImpl_funcs {
  
  template<typename F, typename... A>
  auto invoke(F f, A&&... args) {
    return compat::apply(f, (UnitImpl*)this, std::forward_as_tuple(std::forward<A>(args)...));
  }
  
  UnitImpl* unitImpl() {
    return (UnitImpl*)this;
  }
  
  void canAttack() {
    throw std::runtime_error("canAttack");
  }
  void canAttackGrouped() {
    throw std::runtime_error("canAttackGrouped");
  }
  void canSetRallyPoint() {
    throw std::runtime_error("canSetRallyPoint");
  }
  void canRightClick() {
    throw std::runtime_error("canRightClick");
  }
  void canRightClickGrouped() {
    throw std::runtime_error("canRightClickGrouped");
  }
  void canUseTech() {
    throw std::runtime_error("canUseTech");
  }
  
};

struct PlayerImpl_funcs {
  
  template<typename F, typename... A>
  auto invoke(F f, A&&... args) {
    return compat::apply(f, (PlayerImpl*)this, std::forward_as_tuple(std::forward<A>(args)...));
  }
  
  PlayerImpl* playerImpl() {
    return (PlayerImpl*)this;
  }
  
  void* getUnits() {
    return retset(gameImplImpl->playerUnits.at(playerImpl()->getIndex()), playerImpl()->getUnits());
  }
  
};

}

class AIModuleWrapper: public AIModule {
public:
  AIModule* m;
  AIModuleWrapper(AIModule* m) : m(m) {}
  virtual ~AIModuleWrapper() {
  }
  
  void import_functions() {
    auto LoadLibrary = compatGameImpl->LoadLibrary;
    auto GetProcAddress = compatGameImpl->GetProcAddress;
    if (!LoadLibrary) throw std::runtime_error("missing LoadLibrary function");
    if (!GetProcAddress) throw std::runtime_error("missing GetProcAddress function");
    void* msvcr120 = LoadLibrary("MSVCR120.dll");
    if (!msvcr120) throw std::runtime_error("failed to load MSVCR120.dll");
    msvcr120_new = (void*(*)(size_t))GetProcAddress(msvcr120, "??2@YAPAXI@Z");
    if (!msvcr120) throw std::runtime_error("failed to import function '??2@YAPAXI@Z' from 'MSVCR120.dll");
    msvcr120_delete = (void(*)(void*))GetProcAddress(msvcr120, "??3@YAXPAX@Z");
    if (!msvcr120) throw std::runtime_error("failed to import function '??3@YAXPAX@Z' from 'MSVCR120.dll");
  }
  
  template<typename... A>
  void vfCall(int n, A... args) {
    compat::call_thiscall((*(void***)m)[n], m, args...);
  }

  virtual void onStart() {
    import_functions();
    gameImplImpl = std::make_unique<GameImplImpl>();
    vfCall(1);
  }

  virtual void onEnd(bool isWinner) {
    vfCall(2, isWinner);
  }

  virtual void onFrame() {
    vfCall(3);
  }

  virtual void onSendText(std::string text) {
  }

  virtual void onReceiveText(Player player, std::string text) {
  }

  virtual void onPlayerLeft(Player player) {
    vfCall(6, fix_pointer(player));
  }

  virtual void onNukeDetect(Position target) {
    vfCall(7, target);
  }

  virtual void onUnitDiscover(Unit unit) {
    vfCall(8, fix_pointer(unit));
  }

  virtual void onUnitEvade(Unit unit) {
    vfCall(9, fix_pointer(unit));
  }

  virtual void onUnitShow(Unit unit) {
    vfCall(10, fix_pointer(unit));
  }

  virtual void onUnitHide(Unit unit) {
    vfCall(11, fix_pointer(unit));
  }

  virtual void onUnitCreate(Unit unit) {
    vfCall(12, fix_pointer(unit));
  }

  virtual void onUnitDestroy(Unit unit) {
    vfCall(13, fix_pointer(unit));
  }

  virtual void onUnitMorph(Unit unit) {
    vfCall(14, fix_pointer(unit));
  }

  virtual void onUnitRenegade(Unit unit) {
    vfCall(15, fix_pointer(unit));
  }
  virtual void onSaveGame(std::string gameName) {
  }

  virtual void onUnitComplete(Unit unit) {
    vfCall(17, fix_pointer(unit));
  }
};


AIModule* CompatGameImpl::wrapAIModule(AIModule* m) {
  return new AIModuleWrapper(m);
}


void* Game_vftable[0x1000];
void* Unit_vftable[0x1000];
void* Player_vftable[0x1000];
void* Bullet_vftable[0x1000];

CompatGameImpl::CompatGameImpl(GameImpl* gameImpl) : gameImpl(gameImpl) {
  
  compatGameImpl = this;
  vftable = Game_vftable;
  
  compat::generate_functions();
  
  using compat::wrap_thiscall;
  
  Game_vftable[0] = nullptr;
  Game_vftable[1] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getForces, "BWAPI::GameImpl::getForces");
  Game_vftable[2] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getPlayers, "BWAPI::GameImpl::getPlayers");
  Game_vftable[3] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getAllUnits, "BWAPI::GameImpl::getAllUnits");
  Game_vftable[4] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getMinerals, "BWAPI::GameImpl::getMinerals");
  Game_vftable[5] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getGeysers, "BWAPI::GameImpl::getGeysers");
  Game_vftable[6] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getNeutralUnits, "BWAPI::GameImpl::getNeutralUnits");
  Game_vftable[7] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getStaticMinerals, "BWAPI::GameImpl::getStaticMinerals");
  Game_vftable[8] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getStaticGeysers, "BWAPI::GameImpl::getStaticGeysers");
  Game_vftable[9] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getStaticNeutralUnits, "BWAPI::GameImpl::getStaticNeutralUnits");
  Game_vftable[10] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getBullets, "BWAPI::GameImpl::getBullets");
  Game_vftable[11] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getNukeDots, "BWAPI::GameImpl::getNukeDots");
  Game_vftable[12] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getEvents, "BWAPI::GameImpl::getEvents");
  Game_vftable[13] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getForce, "BWAPI::GameImpl::getForce");
  Game_vftable[14] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getPlayer, "BWAPI::GameImpl::getPlayer");
  Game_vftable[15] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getUnit, "BWAPI::GameImpl::getUnit");
  Game_vftable[16] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::indexToUnit, "BWAPI::GameImpl::indexToUnit");
  Game_vftable[17] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getRegion, "BWAPI::GameImpl::getRegion");
  Game_vftable[18] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getGameType, "BWAPI::GameImpl::getGameType");
  Game_vftable[19] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getLatency, "BWAPI::GameImpl::getLatency");
  Game_vftable[20] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getFrameCount, "BWAPI::GameImpl::getFrameCount");
  Game_vftable[21] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getReplayFrameCount, "BWAPI::GameImpl::getReplayFrameCount");
  Game_vftable[22] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getFPS, "BWAPI::GameImpl::getFPS");
  Game_vftable[23] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getAverageFPS, "BWAPI::GameImpl::getAverageFPS");
  Game_vftable[24] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getMousePosition, "BWAPI::GameImpl::getMousePosition");
  Game_vftable[25] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getMouseState, "BWAPI::GameImpl::getMouseState");
  Game_vftable[26] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getKeyState, "BWAPI::GameImpl::getKeyState");
  Game_vftable[27] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getScreenPosition, "BWAPI::GameImpl::getScreenPosition");
  Game_vftable[28] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setScreenPosition, "BWAPI::GameImpl::setScreenPosition");
  Game_vftable[29] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::pingMinimap, "BWAPI::GameImpl::pingMinimap");
  Game_vftable[30] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isFlagEnabled, "BWAPI::GameImpl::isFlagEnabled");
  Game_vftable[31] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::enableFlag, "BWAPI::GameImpl::enableFlag");
  Game_vftable[32] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getUnitsInRectangle, "BWAPI::GameImpl::getUnitsInRectangle");
  Game_vftable[33] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getClosestUnitInRectangle, "BWAPI::GameImpl::getClosestUnitInRectangle");
  Game_vftable[34] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getBestUnit, "BWAPI::GameImpl::getBestUnit");
  Game_vftable[35] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getLastError, "BWAPI::GameImpl::getLastError");
  Game_vftable[36] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setLastError, "BWAPI::GameImpl::setLastError");
  Game_vftable[37] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapWidth, "BWAPI::GameImpl::mapWidth");
  Game_vftable[38] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapHeight, "BWAPI::GameImpl::mapHeight");
  Game_vftable[39] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapFileName, "BWAPI::GameImpl::mapFileName");
  Game_vftable[40] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapPathName, "BWAPI::GameImpl::mapPathName");
  Game_vftable[41] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapName, "BWAPI::GameImpl::mapName");
  Game_vftable[42] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::mapHash, "BWAPI::GameImpl::mapHash");
  Game_vftable[43] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isWalkable, "BWAPI::GameImpl::isWalkable");
  Game_vftable[44] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getGroundHeight, "BWAPI::GameImpl::getGroundHeight");
  Game_vftable[45] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isBuildable, "BWAPI::GameImpl::isBuildable");
  Game_vftable[46] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isVisible, "BWAPI::GameImpl::isVisible");
  Game_vftable[47] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isExplored, "BWAPI::GameImpl::isExplored");
  Game_vftable[48] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::hasCreep, "BWAPI::GameImpl::hasCreep");
  Game_vftable[49] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::hasPowerPrecise, "BWAPI::GameImpl::hasPowerPrecise");
  Game_vftable[50] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::canBuildHere, "BWAPI::GameImpl::canBuildHere");
  Game_vftable[51] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::canMake, "BWAPI::GameImpl::canMake");
  Game_vftable[52] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::canResearch, "BWAPI::GameImpl::canResearch");
  Game_vftable[53] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::canUpgrade, "BWAPI::GameImpl::canUpgrade");
  Game_vftable[54] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::getStartLocations, "BWAPI::GameImpl::getStartLocations");
  Game_vftable[55] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::vPrintf, "BWAPI::GameImpl::vPrintf");
  Game_vftable[56] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::vSendTextEx, "BWAPI::GameImpl::vSendTextEx");
  Game_vftable[57] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isInGame, "BWAPI::GameImpl::isInGame");
  Game_vftable[58] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isMultiplayer, "BWAPI::GameImpl::isMultiplayer");
  Game_vftable[59] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isBattleNet, "BWAPI::GameImpl::isBattleNet");
  Game_vftable[60] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isPaused, "BWAPI::GameImpl::isPaused");
  Game_vftable[61] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isReplay, "BWAPI::GameImpl::isReplay");
  Game_vftable[62] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::pauseGame, "BWAPI::GameImpl::pauseGame");
  Game_vftable[63] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::resumeGame, "BWAPI::GameImpl::resumeGame");
  Game_vftable[64] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::leaveGame, "BWAPI::GameImpl::leaveGame");
  Game_vftable[65] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::restartGame, "BWAPI::GameImpl::restartGame");
  Game_vftable[66] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setLocalSpeed, "BWAPI::GameImpl::setLocalSpeed");
  Game_vftable[67] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::issueCommand, "BWAPI::GameImpl::issueCommand");
  Game_vftable[68] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getSelectedUnits, "BWAPI::GameImpl::getSelectedUnits");
  Game_vftable[69] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::self, "BWAPI::GameImpl::self");
  Game_vftable[70] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::enemy, "BWAPI::GameImpl::enemy");
  Game_vftable[71] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::neutral, "BWAPI::GameImpl::neutral");
  Game_vftable[72] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::allies, "BWAPI::GameImpl::allies");
  Game_vftable[73] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::enemies, "BWAPI::GameImpl::enemies");
  Game_vftable[74] = wrap_thiscall<CompatGameImpl>(&GameImpl_funcs::observers, "BWAPI::GameImpl::observers");
  Game_vftable[75] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setTextSize, "BWAPI::GameImpl::setTextSize");
  Game_vftable[76] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::vDrawText, "BWAPI::GameImpl::vDrawText");
  Game_vftable[77] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawBox, "BWAPI::GameImpl::drawBox");
  Game_vftable[78] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawTriangle, "BWAPI::GameImpl::drawTriangle");
  Game_vftable[79] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawCircle, "BWAPI::GameImpl::drawCircle");
  Game_vftable[80] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawEllipse, "BWAPI::GameImpl::drawEllipse");
  Game_vftable[81] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawDot, "BWAPI::GameImpl::drawDot");
  Game_vftable[82] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::drawLine, "BWAPI::GameImpl::drawLine");
  Game_vftable[83] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getLatencyFrames, "BWAPI::GameImpl::getLatencyFrames");
  Game_vftable[84] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getLatencyTime, "BWAPI::GameImpl::getLatencyTime");
  Game_vftable[85] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getRemainingLatencyFrames, "BWAPI::GameImpl::getRemainingLatencyFrames");
  Game_vftable[86] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getRemainingLatencyTime, "BWAPI::GameImpl::getRemainingLatencyTime");
  Game_vftable[87] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getRevision, "BWAPI::GameImpl::getRevision");
  Game_vftable[88] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isDebug, "BWAPI::GameImpl::isDebug");
  Game_vftable[89] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isLatComEnabled, "BWAPI::GameImpl::isLatComEnabled");
  Game_vftable[90] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setLatCom, "BWAPI::GameImpl::setLatCom");
  Game_vftable[91] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::isGUIEnabled, "BWAPI::GameImpl::isGUIEnabled");
  Game_vftable[92] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setGUI, "BWAPI::GameImpl::setGUI");
  Game_vftable[93] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getInstanceNumber, "BWAPI::GameImpl::getInstanceNumber");
  Game_vftable[94] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getAPM, "BWAPI::GameImpl::getAPM");
  Game_vftable[95] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setMap, "BWAPI::GameImpl::setMap");
  Game_vftable[96] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setFrameSkip, "BWAPI::GameImpl::setFrameSkip");
  Game_vftable[97] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setAlliance, "BWAPI::GameImpl::setAlliance");
  Game_vftable[98] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setVision, "BWAPI::GameImpl::setVision");
  Game_vftable[99] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::elapsedTime, "BWAPI::GameImpl::elapsedTime");
  Game_vftable[100] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setCommandOptimizationLevel, "BWAPI::GameImpl::setCommandOptimizationLevel");
  Game_vftable[101] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::countdownTimer, "BWAPI::GameImpl::countdownTimer");
  Game_vftable[102] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getAllRegions, "BWAPI::GameImpl::getAllRegions");
  Game_vftable[103] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getRegionAt, "BWAPI::GameImpl::getRegionAt");
  Game_vftable[104] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::getLastEventTime, "BWAPI::GameImpl::getLastEventTime");
  Game_vftable[105] = wrap_thiscall<CompatGameImpl>(&BWAPI::GameImpl::setRevealAll, "BWAPI::GameImpl::setRevealAll");
  
  Unit_vftable[0] = nullptr;
  Unit_vftable[1] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getID, "BWAPI::UnitImpl::getID");
  Unit_vftable[2] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::exists, "BWAPI::UnitImpl::exists");
  Unit_vftable[3] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getReplayID, "BWAPI::UnitImpl::getReplayID");
  Unit_vftable[4] = wrap_thiscall<CompatUnitImpl>((class BWAPI::PlayerInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getPlayer, "BWAPI::UnitImpl::getPlayer");
  Unit_vftable[5] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitType (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getType, "BWAPI::UnitImpl::getType");
  Unit_vftable[6] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,1> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getPosition, "BWAPI::UnitImpl::getPosition");
  Unit_vftable[7] = wrap_thiscall<CompatUnitImpl>((double (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getAngle, "BWAPI::UnitImpl::getAngle");
  Unit_vftable[8] = wrap_thiscall<CompatUnitImpl>((double (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getVelocityX, "BWAPI::UnitImpl::getVelocityX");
  Unit_vftable[9] = wrap_thiscall<CompatUnitImpl>((double (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getVelocityY, "BWAPI::UnitImpl::getVelocityY");
  Unit_vftable[10] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getHitPoints, "BWAPI::UnitImpl::getHitPoints");
  Unit_vftable[11] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getShields, "BWAPI::UnitImpl::getShields");
  Unit_vftable[12] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getEnergy, "BWAPI::UnitImpl::getEnergy");
  Unit_vftable[13] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getResources, "BWAPI::UnitImpl::getResources");
  Unit_vftable[14] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getResourceGroup, "BWAPI::UnitImpl::getResourceGroup");
  Unit_vftable[15] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getLastCommandFrame, "BWAPI::UnitImpl::getLastCommandFrame");
  Unit_vftable[16] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitCommand (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getLastCommand, "BWAPI::UnitImpl::getLastCommand");
  Unit_vftable[17] = wrap_thiscall<CompatUnitImpl>((class BWAPI::PlayerInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getLastAttackingPlayer, "BWAPI::UnitImpl::getLastAttackingPlayer");
  Unit_vftable[18] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitType (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInitialType, "BWAPI::UnitImpl::getInitialType");
  Unit_vftable[19] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,1> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInitialPosition, "BWAPI::UnitImpl::getInitialPosition");
  Unit_vftable[20] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,32> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInitialTilePosition, "BWAPI::UnitImpl::getInitialTilePosition");
  Unit_vftable[21] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInitialHitPoints, "BWAPI::UnitImpl::getInitialHitPoints");
  Unit_vftable[22] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInitialResources, "BWAPI::UnitImpl::getInitialResources");
  Unit_vftable[23] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getKillCount, "BWAPI::UnitImpl::getKillCount");
  Unit_vftable[24] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getAcidSporeCount, "BWAPI::UnitImpl::getAcidSporeCount");
  Unit_vftable[25] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getInterceptorCount, "BWAPI::UnitImpl::getInterceptorCount");
  Unit_vftable[26] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getScarabCount, "BWAPI::UnitImpl::getScarabCount");
  Unit_vftable[27] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getSpiderMineCount, "BWAPI::UnitImpl::getSpiderMineCount");
  Unit_vftable[28] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getGroundWeaponCooldown, "BWAPI::UnitImpl::getGroundWeaponCooldown");
  Unit_vftable[29] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getAirWeaponCooldown, "BWAPI::UnitImpl::getAirWeaponCooldown");
  Unit_vftable[30] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getSpellCooldown, "BWAPI::UnitImpl::getSpellCooldown");
  Unit_vftable[31] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getDefenseMatrixPoints, "BWAPI::UnitImpl::getDefenseMatrixPoints");
  Unit_vftable[32] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getDefenseMatrixTimer, "BWAPI::UnitImpl::getDefenseMatrixTimer");
  Unit_vftable[33] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getEnsnareTimer, "BWAPI::UnitImpl::getEnsnareTimer");
  Unit_vftable[34] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getIrradiateTimer, "BWAPI::UnitImpl::getIrradiateTimer");
  Unit_vftable[35] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getLockdownTimer, "BWAPI::UnitImpl::getLockdownTimer");
  Unit_vftable[36] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getMaelstromTimer, "BWAPI::UnitImpl::getMaelstromTimer");
  Unit_vftable[37] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getOrderTimer, "BWAPI::UnitImpl::getOrderTimer");
  Unit_vftable[38] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getPlagueTimer, "BWAPI::UnitImpl::getPlagueTimer");
  Unit_vftable[39] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRemoveTimer, "BWAPI::UnitImpl::getRemoveTimer");
  Unit_vftable[40] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getStasisTimer, "BWAPI::UnitImpl::getStasisTimer");
  Unit_vftable[41] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getStimTimer, "BWAPI::UnitImpl::getStimTimer");
  Unit_vftable[42] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitType (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getBuildType, "BWAPI::UnitImpl::getBuildType");
  Unit_vftable[43] = wrap_thiscall<CompatUnitImpl>(&BWAPI::UnitImpl::getTrainingQueue, "BWAPI::UnitImpl::getTrainingQueue");
  Unit_vftable[44] = wrap_thiscall<CompatUnitImpl>((class BWAPI::TechType (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getTech, "BWAPI::UnitImpl::getTech");
  Unit_vftable[45] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UpgradeType (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getUpgrade, "BWAPI::UnitImpl::getUpgrade");
  Unit_vftable[46] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRemainingBuildTime, "BWAPI::UnitImpl::getRemainingBuildTime");
  Unit_vftable[47] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRemainingTrainTime, "BWAPI::UnitImpl::getRemainingTrainTime");
  Unit_vftable[48] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRemainingResearchTime, "BWAPI::UnitImpl::getRemainingResearchTime");
  Unit_vftable[49] = wrap_thiscall<CompatUnitImpl>((int (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRemainingUpgradeTime, "BWAPI::UnitImpl::getRemainingUpgradeTime");
  Unit_vftable[50] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getBuildUnit, "BWAPI::UnitImpl::getBuildUnit");
  Unit_vftable[51] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getTarget, "BWAPI::UnitImpl::getTarget");
  Unit_vftable[52] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,1> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getTargetPosition, "BWAPI::UnitImpl::getTargetPosition");
  Unit_vftable[53] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Order (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getOrder, "BWAPI::UnitImpl::getOrder");
  Unit_vftable[54] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Order (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getSecondaryOrder, "BWAPI::UnitImpl::getSecondaryOrder");
  Unit_vftable[55] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getOrderTarget, "BWAPI::UnitImpl::getOrderTarget");
  Unit_vftable[56] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,1> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getOrderTargetPosition, "BWAPI::UnitImpl::getOrderTargetPosition");
  Unit_vftable[57] = wrap_thiscall<CompatUnitImpl>((class BWAPI::Point<int,1> (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRallyPosition, "BWAPI::UnitImpl::getRallyPosition");
  Unit_vftable[58] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getRallyUnit, "BWAPI::UnitImpl::getRallyUnit");
  Unit_vftable[59] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getAddon, "BWAPI::UnitImpl::getAddon");
  Unit_vftable[60] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getNydusExit, "BWAPI::UnitImpl::getNydusExit");
  Unit_vftable[61] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getPowerUp, "BWAPI::UnitImpl::getPowerUp");
  Unit_vftable[62] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getTransport, "BWAPI::UnitImpl::getTransport");
  Unit_vftable[63] = wrap_thiscall<CompatUnitImpl>(&BWAPI::UnitImpl::getLoadedUnits, "BWAPI::UnitImpl::getLoadedUnits");
  Unit_vftable[64] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getCarrier, "BWAPI::UnitImpl::getCarrier");
  Unit_vftable[65] = wrap_thiscall<CompatUnitImpl>(&BWAPI::UnitImpl::getInterceptors, "BWAPI::UnitImpl::getInterceptors");
  Unit_vftable[66] = wrap_thiscall<CompatUnitImpl>((class BWAPI::UnitInterface * (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::getHatchery, "BWAPI::UnitImpl::getHatchery");
  Unit_vftable[67] = wrap_thiscall<CompatUnitImpl>(&BWAPI::UnitImpl::getLarva, "BWAPI::UnitImpl::getLarva");
  Unit_vftable[68] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::hasNuke, "BWAPI::UnitImpl::hasNuke");
  Unit_vftable[69] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isAccelerating, "BWAPI::UnitImpl::isAccelerating");
  Unit_vftable[70] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isAttacking, "BWAPI::UnitImpl::isAttacking");
  Unit_vftable[71] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isAttackFrame, "BWAPI::UnitImpl::isAttackFrame");
  Unit_vftable[72] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isBeingGathered, "BWAPI::UnitImpl::isBeingGathered");
  Unit_vftable[73] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isBeingHealed, "BWAPI::UnitImpl::isBeingHealed");
  Unit_vftable[74] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isBlind, "BWAPI::UnitImpl::isBlind");
  Unit_vftable[75] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isBraking, "BWAPI::UnitImpl::isBraking");
  Unit_vftable[76] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isBurrowed, "BWAPI::UnitImpl::isBurrowed");
  Unit_vftable[77] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isCarryingGas, "BWAPI::UnitImpl::isCarryingGas");
  Unit_vftable[78] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isCarryingMinerals, "BWAPI::UnitImpl::isCarryingMinerals");
  Unit_vftable[79] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isCloaked, "BWAPI::UnitImpl::isCloaked");
  Unit_vftable[80] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isCompleted, "BWAPI::UnitImpl::isCompleted");
  Unit_vftable[81] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isConstructing, "BWAPI::UnitImpl::isConstructing");
  Unit_vftable[82] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isDetected, "BWAPI::UnitImpl::isDetected");
  Unit_vftable[83] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isGatheringGas, "BWAPI::UnitImpl::isGatheringGas");
  Unit_vftable[84] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isGatheringMinerals, "BWAPI::UnitImpl::isGatheringMinerals");
  Unit_vftable[85] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isHallucination, "BWAPI::UnitImpl::isHallucination");
  Unit_vftable[86] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isIdle, "BWAPI::UnitImpl::isIdle");
  Unit_vftable[87] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isInterruptible, "BWAPI::UnitImpl::isInterruptible");
  Unit_vftable[88] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isInvincible, "BWAPI::UnitImpl::isInvincible");
  Unit_vftable[89] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isLifted, "BWAPI::UnitImpl::isLifted");
  Unit_vftable[90] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isMorphing, "BWAPI::UnitImpl::isMorphing");
  Unit_vftable[91] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isMoving, "BWAPI::UnitImpl::isMoving");
  Unit_vftable[92] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isParasited, "BWAPI::UnitImpl::isParasited");
  Unit_vftable[93] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isSelected, "BWAPI::UnitImpl::isSelected");
  Unit_vftable[94] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isStartingAttack, "BWAPI::UnitImpl::isStartingAttack");
  Unit_vftable[95] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isStuck, "BWAPI::UnitImpl::isStuck");
  Unit_vftable[96] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isTraining, "BWAPI::UnitImpl::isTraining");
  Unit_vftable[97] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isUnderAttack, "BWAPI::UnitImpl::isUnderAttack");
  Unit_vftable[98] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isUnderDarkSwarm, "BWAPI::UnitImpl::isUnderDarkSwarm");
  Unit_vftable[99] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isUnderDisruptionWeb, "BWAPI::UnitImpl::isUnderDisruptionWeb");
  Unit_vftable[100] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isUnderStorm, "BWAPI::UnitImpl::isUnderStorm");
  Unit_vftable[101] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isPowered, "BWAPI::UnitImpl::isPowered");
  Unit_vftable[102] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::PlayerInterface *)const)&BWAPI::UnitImpl::isVisible, "BWAPI::UnitImpl::isVisible");
  Unit_vftable[103] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::isTargetable, "BWAPI::UnitImpl::isTargetable");
  Unit_vftable[104] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitCommand) )&BWAPI::UnitImpl::issueCommand, "BWAPI::UnitImpl::issueCommand");
  Unit_vftable[105] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitCommand,bool,bool,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canIssueCommand, "BWAPI::UnitImpl::canIssueCommand");
  Unit_vftable[106] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitCommand,bool,bool,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canIssueCommandGrouped, "BWAPI::UnitImpl::canIssueCommandGrouped");
  Unit_vftable[107] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(void)const)&BWAPI::UnitImpl::canCommand, "BWAPI::UnitImpl::canCommand");
  Unit_vftable[108] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCommandGrouped, "BWAPI::UnitImpl::canCommandGrouped");
  Unit_vftable[109] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitCommandType,bool)const)&BWAPI::UnitImpl::canIssueCommandType, "BWAPI::UnitImpl::canIssueCommandType");
  Unit_vftable[110] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitCommandType,bool,bool)const)&BWAPI::UnitImpl::canIssueCommandTypeGrouped, "BWAPI::UnitImpl::canIssueCommandTypeGrouped");
  Unit_vftable[111] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool)const)&BWAPI::UnitImpl::canTargetUnit, "BWAPI::UnitImpl::canTargetUnit");
  Unit_vftable[112] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canAttack, "BWAPI::UnitImpl::canAttack");
  Unit_vftable[113] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canAttack, "BWAPI::UnitImpl::canAttack");
  Unit_vftable[114] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canAttackGrouped, "BWAPI::UnitImpl::canAttackGrouped");
  Unit_vftable[115] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canAttackGrouped, "BWAPI::UnitImpl::canAttackGrouped");
  Unit_vftable[116] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canAttackMove, "BWAPI::UnitImpl::canAttackMove");
  Unit_vftable[117] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canAttackMoveGrouped, "BWAPI::UnitImpl::canAttackMoveGrouped");
  Unit_vftable[118] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canAttackUnit, "BWAPI::UnitImpl::canAttackUnit");
  Unit_vftable[119] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canAttackUnit, "BWAPI::UnitImpl::canAttackUnit");
  Unit_vftable[120] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canAttackUnitGrouped, "BWAPI::UnitImpl::canAttackUnitGrouped");
  Unit_vftable[121] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canAttackUnitGrouped, "BWAPI::UnitImpl::canAttackUnitGrouped");
  Unit_vftable[122] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitType,class BWAPI::Point<int,32>,bool,bool,bool)const)&BWAPI::UnitImpl::canBuild, "BWAPI::UnitImpl::canBuild");
  Unit_vftable[123] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitType,bool,bool)const)&BWAPI::UnitImpl::canBuild, "BWAPI::UnitImpl::canBuild");
  Unit_vftable[124] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canBuild, "BWAPI::UnitImpl::canBuild");
  Unit_vftable[125] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitType,bool,bool)const)&BWAPI::UnitImpl::canBuildAddon, "BWAPI::UnitImpl::canBuildAddon");
  Unit_vftable[126] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canBuildAddon, "BWAPI::UnitImpl::canBuildAddon");
  Unit_vftable[127] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitType,bool,bool)const)&BWAPI::UnitImpl::canTrain, "BWAPI::UnitImpl::canTrain");
  Unit_vftable[128] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canTrain, "BWAPI::UnitImpl::canTrain");
  Unit_vftable[129] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitType,bool,bool)const)&BWAPI::UnitImpl::canMorph, "BWAPI::UnitImpl::canMorph");
  Unit_vftable[130] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canMorph, "BWAPI::UnitImpl::canMorph");
  Unit_vftable[131] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,bool)const)&BWAPI::UnitImpl::canResearch, "BWAPI::UnitImpl::canResearch");
  Unit_vftable[132] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUpgrade, "BWAPI::UnitImpl::canUpgrade");
  Unit_vftable[133] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UpgradeType,bool)const)&BWAPI::UnitImpl::canUpgrade, "BWAPI::UnitImpl::canUpgrade");
  Unit_vftable[134] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUpgrade, "BWAPI::UnitImpl::canUpgrade");
  Unit_vftable[135] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canSetRallyPoint, "BWAPI::UnitImpl::canSetRallyPoint");
  Unit_vftable[136] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canSetRallyPoint, "BWAPI::UnitImpl::canSetRallyPoint");
  Unit_vftable[137] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canSetRallyUnit, "BWAPI::UnitImpl::canSetRallyUnit");
  Unit_vftable[138] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canSetRallyUnit, "BWAPI::UnitImpl::canSetRallyUnit");
  Unit_vftable[139] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canSetRallyUnit, "BWAPI::UnitImpl::canSetRallyUnit");
  Unit_vftable[140] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canMove, "BWAPI::UnitImpl::canMove");
  Unit_vftable[141] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canMoveGrouped, "BWAPI::UnitImpl::canMoveGrouped");
  Unit_vftable[142] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canFollow, "BWAPI::UnitImpl::canFollow");
  Unit_vftable[143] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canPatrolGrouped, "BWAPI::UnitImpl::canPatrolGrouped");
  Unit_vftable[144] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canFollow, "BWAPI::UnitImpl::canFollow");
  Unit_vftable[145] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canFollow, "BWAPI::UnitImpl::canFollow");
  Unit_vftable[146] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canGather, "BWAPI::UnitImpl::canGather");
  Unit_vftable[147] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canGather, "BWAPI::UnitImpl::canGather");
  Unit_vftable[148] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canReturnCargo, "BWAPI::UnitImpl::canReturnCargo");
  Unit_vftable[149] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canHoldPosition, "BWAPI::UnitImpl::canHoldPosition");
  Unit_vftable[150] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canStop, "BWAPI::UnitImpl::canStop");
  Unit_vftable[151] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canRepair, "BWAPI::UnitImpl::canRepair");
  Unit_vftable[152] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canRepair, "BWAPI::UnitImpl::canRepair");
  Unit_vftable[153] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canBurrow, "BWAPI::UnitImpl::canBurrow");
  Unit_vftable[154] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnburrow, "BWAPI::UnitImpl::canUnburrow");
  Unit_vftable[155] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCloak, "BWAPI::UnitImpl::canCloak");
  Unit_vftable[156] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canDecloak, "BWAPI::UnitImpl::canDecloak");
  Unit_vftable[157] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canSiege, "BWAPI::UnitImpl::canSiege");
  Unit_vftable[158] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnsiege, "BWAPI::UnitImpl::canUnsiege");
  Unit_vftable[159] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canLift, "BWAPI::UnitImpl::canLift");
  Unit_vftable[160] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::Point<int,32>,bool,bool)const)&BWAPI::UnitImpl::canLand, "BWAPI::UnitImpl::canLand");
  Unit_vftable[161] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canLand, "BWAPI::UnitImpl::canLand");
  Unit_vftable[162] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canLoad, "BWAPI::UnitImpl::canLoad");
  Unit_vftable[163] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canLoad, "BWAPI::UnitImpl::canLoad");
  Unit_vftable[164] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnloadWithOrWithoutTarget, "BWAPI::UnitImpl::canUnloadWithOrWithoutTarget");
  Unit_vftable[165] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::Point<int,1>,bool,bool)const)&BWAPI::UnitImpl::canUnloadAtPosition, "BWAPI::UnitImpl::canUnloadAtPosition");
  Unit_vftable[166] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canUnload, "BWAPI::UnitImpl::canUnload");
  Unit_vftable[167] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnload, "BWAPI::UnitImpl::canUnload");
  Unit_vftable[168] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnload, "BWAPI::UnitImpl::canUnload");
  Unit_vftable[169] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::Point<int,1>,bool,bool)const)&BWAPI::UnitImpl::canUnloadAllPosition, "BWAPI::UnitImpl::canUnloadAllPosition");
  Unit_vftable[170] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUnloadAllPosition, "BWAPI::UnitImpl::canUnloadAllPosition");
  Unit_vftable[171] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canRightClick, "BWAPI::UnitImpl::canRightClick");
  Unit_vftable[172] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canRightClick, "BWAPI::UnitImpl::canRightClick");
  Unit_vftable[173] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canRightClickGrouped, "BWAPI::UnitImpl::canRightClickGrouped");
  Unit_vftable[174] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canRightClickGrouped, "BWAPI::UnitImpl::canRightClickGrouped");
  Unit_vftable[175] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canRightClickPosition, "BWAPI::UnitImpl::canRightClickPosition");
  Unit_vftable[176] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canRightClickPositionGrouped, "BWAPI::UnitImpl::canRightClickPositionGrouped");
  Unit_vftable[177] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool)const)&BWAPI::UnitImpl::canRightClickUnit, "BWAPI::UnitImpl::canRightClickUnit");
  Unit_vftable[178] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canRightClickUnit, "BWAPI::UnitImpl::canRightClickUnit");
  Unit_vftable[179] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::UnitInterface *,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canRightClickUnitGrouped, "BWAPI::UnitImpl::canRightClickUnitGrouped");
  Unit_vftable[180] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool,bool)const)&BWAPI::UnitImpl::canRightClickUnitGrouped, "BWAPI::UnitImpl::canRightClickUnitGrouped");
  Unit_vftable[181] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canHaltConstruction, "BWAPI::UnitImpl::canHaltConstruction");
  Unit_vftable[182] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelConstruction, "BWAPI::UnitImpl::canCancelConstruction");
  Unit_vftable[183] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelAddon, "BWAPI::UnitImpl::canCancelAddon");
  Unit_vftable[184] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelTrain, "BWAPI::UnitImpl::canCancelTrain");
  Unit_vftable[185] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(int,bool,bool)const)&BWAPI::UnitImpl::canCancelTrainSlot, "BWAPI::UnitImpl::canCancelTrainSlot");
  Unit_vftable[186] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelTrain, "BWAPI::UnitImpl::canCancelTrain");
  Unit_vftable[187] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelMorph, "BWAPI::UnitImpl::canCancelMorph");
  Unit_vftable[188] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelResearch, "BWAPI::UnitImpl::canCancelResearch");
  Unit_vftable[189] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canCancelUpgrade, "BWAPI::UnitImpl::canCancelUpgrade");
  Unit_vftable[190] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,bool,bool)const)&BWAPI::UnitImpl::canUseTechWithOrWithoutTarget, "BWAPI::UnitImpl::canUseTechWithOrWithoutTarget");
  Unit_vftable[191] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canUseTechWithOrWithoutTarget, "BWAPI::UnitImpl::canUseTechWithOrWithoutTarget");
  Unit_vftable[192] = wrap_thiscall<CompatUnitImpl>(&UnitImpl_funcs::canUseTech, "BWAPI::UnitImpl::canUseTech");
  Unit_vftable[193] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,bool,bool)const)&BWAPI::UnitImpl::canUseTechWithoutTarget, "BWAPI::UnitImpl::canUseTechWithoutTarget");
  Unit_vftable[194] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,class BWAPI::UnitInterface *,bool,bool,bool,bool)const)&BWAPI::UnitImpl::canUseTechUnit, "BWAPI::UnitImpl::canUseTechUnit");
  Unit_vftable[195] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,bool,bool)const)&BWAPI::UnitImpl::canUseTechUnit, "BWAPI::UnitImpl::canUseTechUnit");
  Unit_vftable[196] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,class BWAPI::Point<int,1>,bool,bool,bool)const)&BWAPI::UnitImpl::canUseTechPosition, "BWAPI::UnitImpl::canUseTechPosition");
  Unit_vftable[197] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::TechType,bool,bool)const)&BWAPI::UnitImpl::canUseTechPosition, "BWAPI::UnitImpl::canUseTechPosition");
  Unit_vftable[198] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(class BWAPI::Point<int,32>,bool,bool)const)&BWAPI::UnitImpl::canPlaceCOP, "BWAPI::UnitImpl::canPlaceCOP");
  Unit_vftable[199] = wrap_thiscall<CompatUnitImpl>((bool (BWAPI::UnitImpl::*)(bool)const)&BWAPI::UnitImpl::canPlaceCOP, "BWAPI::UnitImpl::canPlaceCOP");
  
  Player_vftable[0] = nullptr;
  Player_vftable[1] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getID, "BWAPI::PlayerImpl::getID");
  Player_vftable[2] = wrap_thiscall<CompatPlayerImpl>((class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> > (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getName, "BWAPI::PlayerImpl::getName");
  Player_vftable[3] = wrap_thiscall<CompatPlayerImpl>(&PlayerImpl_funcs::getUnits, "BWAPI::PlayerImpl::getUnits");
  Player_vftable[4] = wrap_thiscall<CompatPlayerImpl>((class BWAPI::Race (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getRace, "BWAPI::PlayerImpl::getRace");
  Player_vftable[5] = wrap_thiscall<CompatPlayerImpl>((class BWAPI::PlayerType (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getType, "BWAPI::PlayerImpl::getType");
  Player_vftable[6] = wrap_thiscall<CompatPlayerImpl>((class BWAPI::ForceInterface * (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getForce, "BWAPI::PlayerImpl::getForce");
  Player_vftable[7] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::PlayerInterface * const)const)&BWAPI::PlayerImpl::isAlly, "BWAPI::PlayerImpl::isAlly");
  Player_vftable[8] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::PlayerInterface * const)const)&BWAPI::PlayerImpl::isEnemy, "BWAPI::PlayerImpl::isEnemy");
  Player_vftable[9] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::isNeutral, "BWAPI::PlayerImpl::isNeutral");
  Player_vftable[10] = wrap_thiscall<CompatPlayerImpl>((class BWAPI::Point<int,32> (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getStartLocation, "BWAPI::PlayerImpl::getStartLocation");
  Player_vftable[11] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::isVictorious, "BWAPI::PlayerImpl::isVictorious");
  Player_vftable[12] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::isDefeated, "BWAPI::PlayerImpl::isDefeated");
  Player_vftable[13] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::leftGame, "BWAPI::PlayerImpl::leftGame");
  Player_vftable[14] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::minerals, "BWAPI::PlayerImpl::minerals");
  Player_vftable[15] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::gas, "BWAPI::PlayerImpl::gas");
  Player_vftable[16] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::gatheredMinerals, "BWAPI::PlayerImpl::gatheredMinerals");
  Player_vftable[17] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::gatheredGas, "BWAPI::PlayerImpl::gatheredGas");
  Player_vftable[18] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::repairedMinerals, "BWAPI::PlayerImpl::repairedMinerals");
  Player_vftable[19] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::repairedGas, "BWAPI::PlayerImpl::repairedGas");
  Player_vftable[20] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::refundedMinerals, "BWAPI::PlayerImpl::refundedMinerals");
  Player_vftable[21] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::refundedGas, "BWAPI::PlayerImpl::refundedGas");
  Player_vftable[22] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::spentMinerals, "BWAPI::PlayerImpl::spentMinerals");
  Player_vftable[23] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::spentGas, "BWAPI::PlayerImpl::spentGas");
  Player_vftable[24] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::Race)const)&BWAPI::PlayerImpl::supplyTotal, "BWAPI::PlayerImpl::supplyTotal");
  Player_vftable[25] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::Race)const)&BWAPI::PlayerImpl::supplyUsed, "BWAPI::PlayerImpl::supplyUsed");
  Player_vftable[26] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::allUnitCount, "BWAPI::PlayerImpl::allUnitCount");
  Player_vftable[27] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::visibleUnitCount, "BWAPI::PlayerImpl::visibleUnitCount");
  Player_vftable[28] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::completedUnitCount, "BWAPI::PlayerImpl::completedUnitCount");
  Player_vftable[29] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::deadUnitCount, "BWAPI::PlayerImpl::deadUnitCount");
  Player_vftable[30] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::killedUnitCount, "BWAPI::PlayerImpl::killedUnitCount");
  Player_vftable[31] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UpgradeType)const)&BWAPI::PlayerImpl::getUpgradeLevel, "BWAPI::PlayerImpl::getUpgradeLevel");
  Player_vftable[32] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::TechType)const)&BWAPI::PlayerImpl::hasResearched, "BWAPI::PlayerImpl::hasResearched");
  Player_vftable[33] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::TechType)const)&BWAPI::PlayerImpl::isResearching, "BWAPI::PlayerImpl::isResearching");
  Player_vftable[34] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::UpgradeType)const)&BWAPI::PlayerImpl::isUpgrading, "BWAPI::PlayerImpl::isUpgrading");
  Player_vftable[35] = wrap_thiscall<CompatPlayerImpl>((class BWAPI::Color (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getColor, "BWAPI::PlayerImpl::getColor");
  Player_vftable[36] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getUnitScore, "BWAPI::PlayerImpl::getUnitScore");
  Player_vftable[37] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getKillScore, "BWAPI::PlayerImpl::getKillScore");
  Player_vftable[38] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getBuildingScore, "BWAPI::PlayerImpl::getBuildingScore");
  Player_vftable[39] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getRazingScore, "BWAPI::PlayerImpl::getRazingScore");
  Player_vftable[40] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::getCustomScore, "BWAPI::PlayerImpl::getCustomScore");
  Player_vftable[41] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(void)const)&BWAPI::PlayerImpl::isObserver, "BWAPI::PlayerImpl::isObserver");
  Player_vftable[42] = wrap_thiscall<CompatPlayerImpl>((int (BWAPI::PlayerImpl::*)(class BWAPI::UpgradeType)const)&BWAPI::PlayerImpl::getMaxUpgradeLevel, "BWAPI::PlayerImpl::getMaxUpgradeLevel");
  Player_vftable[43] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::TechType)const)&BWAPI::PlayerImpl::isResearchAvailable, "BWAPI::PlayerImpl::isResearchAvailable");
  Player_vftable[44] = wrap_thiscall<CompatPlayerImpl>((bool (BWAPI::PlayerImpl::*)(class BWAPI::UnitType)const)&BWAPI::PlayerImpl::isUnitAvailable, "BWAPI::PlayerImpl::isUnitAvailable");
  
  Bullet_vftable[0] = nullptr;
  Bullet_vftable[1] = wrap_thiscall<CompatBulletImpl>((int (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getID, "BWAPI::BulletImpl::getID");
  Bullet_vftable[2] = wrap_thiscall<CompatBulletImpl>((bool (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::exists, "BWAPI::BulletImpl::exists");
  Bullet_vftable[3] = wrap_thiscall<CompatBulletImpl>((class BWAPI::PlayerInterface * (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getPlayer, "BWAPI::BulletImpl::getPlayer");
  Bullet_vftable[4] = wrap_thiscall<CompatBulletImpl>((class BWAPI::BulletType (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getType, "BWAPI::BulletImpl::getType");
  Bullet_vftable[5] = wrap_thiscall<CompatBulletImpl>((class BWAPI::UnitInterface * (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getSource, "BWAPI::BulletImpl::getSource");
  Bullet_vftable[6] = wrap_thiscall<CompatBulletImpl>((class BWAPI::Point<int,1> (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getPosition, "BWAPI::BulletImpl::getPosition");
  Bullet_vftable[7] = wrap_thiscall<CompatBulletImpl>((double (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getAngle, "BWAPI::BulletImpl::getAngle");
  Bullet_vftable[8] = wrap_thiscall<CompatBulletImpl>((double (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getVelocityX, "BWAPI::BulletImpl::getVelocityX");
  Bullet_vftable[9] = wrap_thiscall<CompatBulletImpl>((double (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getVelocityY, "BWAPI::BulletImpl::getVelocityY");
  Bullet_vftable[10] = wrap_thiscall<CompatBulletImpl>((class BWAPI::UnitInterface * (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getTarget, "BWAPI::BulletImpl::getTarget");
  Bullet_vftable[11] = wrap_thiscall<CompatBulletImpl>((class BWAPI::Point<int,1> (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getTargetPosition, "BWAPI::BulletImpl::getTargetPosition");
  Bullet_vftable[12] = wrap_thiscall<CompatBulletImpl>((int (BWAPI::BulletImpl::*)(void)const)&BWAPI::BulletImpl::getRemoveTimer, "BWAPI::BulletImpl::getRemoveTimer");
  Bullet_vftable[13] = wrap_thiscall<CompatBulletImpl>((bool (BWAPI::BulletImpl::*)(class BWAPI::PlayerInterface *)const)&BWAPI::BulletImpl::isVisible, "BWAPI::BulletImpl::isVisible");
  
  
}

CompatGameImpl::~CompatGameImpl() {
  
}

CompatUnitImpl::CompatUnitImpl(UnitImpl* impl) : impl(impl) {
  vftable = Unit_vftable;
}

CompatUnitImpl::~CompatUnitImpl() {
  
}

CompatPlayerImpl::CompatPlayerImpl(PlayerImpl* impl) : impl(impl) {
  vftable = Player_vftable;
}

CompatPlayerImpl::~CompatPlayerImpl() {
  
}

CompatBulletImpl::CompatBulletImpl(BulletImpl* impl) : impl(impl) {
  vftable = Bullet_vftable;
}

CompatBulletImpl::~CompatBulletImpl() {
  
}


}

