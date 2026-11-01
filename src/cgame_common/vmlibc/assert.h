#ifndef VM_ASSERT_H
#define VM_ASSERT_H
void _vm_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : _vm_assert_fail(#e, __FILE__, __LINE__))
#endif
#endif
