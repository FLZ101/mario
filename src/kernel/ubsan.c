#include <types.h>
#include <printk.h>

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_type_mismatch_v1(void *data, unsigned long pointer)
{
	hang("ubsan: type mismatch");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_out_of_bounds(void *data, unsigned long index)
{
	hang("ubsan: out of bounds");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_pointer_overflow(void *data, unsigned long base,
				     unsigned long result)
{
	hang("ubsan: pointer overflow");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_shift_out_of_bounds(void *data, unsigned long lhs,
					unsigned long rhs)
{
	hang("ubsan: shift out of bounds");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_type_mismatch_v1_abort(void *data, unsigned long pointer)
{
	hang("ubsan: type mismatch");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_out_of_bounds_abort(void *data, unsigned long index)
{
	hang("ubsan: out of bounds");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_pointer_overflow_abort(void *data, unsigned long base,
					   unsigned long result)
{
	hang("ubsan: pointer overflow");
}

__attribute__((noreturn, no_sanitize_undefined))
void __ubsan_handle_shift_out_of_bounds_abort(void *data, unsigned long lhs,
					      unsigned long rhs)
{
	hang("ubsan: shift out of bounds");
}
