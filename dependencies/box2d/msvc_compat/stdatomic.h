// stdatomic.h - Box2D 3.x C11 原子兼容层(MSVC 无 C11 原子支持)
// 单线程场景直接映射为普通 int; _Atomic 关键字由编译选项 /D_Atomic= 置空。
// 用宏实现(MSVC C 对 inline 未命名参数支持差, 宏最稳)。
// 放置于 dependencies/box2d/msvc_compat/, include 路径优先于系统 stdatomic.h。
#ifndef MIKAN_STDATOMIC_COMPAT_H
#define MIKAN_STDATOMIC_COMPAT_H

typedef int atomic_int;
typedef int atomic_uint;
typedef long atomic_long;

#define ATOMIC_INT_LOCK_FREE 2

typedef int memory_order;
#define memory_order_relaxed 0
#define memory_order_consume 0
#define memory_order_acquire 0
#define memory_order_release 0
#define memory_order_acq_rel 0
#define memory_order_seq_cst 0

#define atomic_load(p)            (*(p))
#define atomic_load_explicit(p,m) (*(p))
#define atomic_store(p,v)         (*(p) = (v))
#define atomic_store_explicit(p,v,m) (*(p) = (v))
#define atomic_fetch_add(p,v)         ((*(p) += (v)) - (v))
#define atomic_fetch_add_explicit(p,v,m) atomic_fetch_add(p, v)
#define atomic_fetch_sub_explicit(p,v,m) ((*(p) -= (v)) + (v))
#define atomic_compare_exchange_strong(p, expected, desired) \
    ((*(p) == *(expected)) ? (*(p) = (desired), 1) : (*(expected) = *(p), 0))

#endif
