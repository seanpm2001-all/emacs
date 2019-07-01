/* Compile byte code produced by bytecomp.el into native code.
   Copyright (C) 2019 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#ifdef HAVE_LIBGCCJIT

#include <stdlib.h>
#include <stdio.h>
#include <libgccjit.h>

#include "lisp.h"
#include "puresize.h"
#include "buffer.h"
#include "bytecode.h"
#include "atimer.h"
#include "window.h"

#define DEFAULT_SPEED 2 /* From 0 to 3 map to gcc -O */

#define COMP_DEBUG 1

#define MAX_FUN_NAME 256

/* Max number of entries of the meta-stack that can get poped.  */

#define MAX_POP 64

#define DISASS_FILE_NAME "emacs-asm.s"

#define CHECK_STACK					\
  eassert (stack >= stack_base && stack < stack_over)

#define PUSH_LVAL(obj)						\
  do {								\
    CHECK_STACK;						\
    emit_assign_to_stack_slot (comp.block,			\
			       stack,				\
			       gcc_jit_lvalue_as_rvalue (obj));	\
    stack++;							\
  } while (0)

#define PUSH_RVAL(obj)						\
  do {								\
    CHECK_STACK;						\
    emit_assign_to_stack_slot (comp.block, stack, (obj));	\
    stack++;							\
  } while (0)

/* This always happens in the first basic block.  */

#define PUSH_PARAM(obj)						\
  do {								\
    CHECK_STACK;						\
    emit_assign_to_stack_slot (prologue,			\
			       stack,				\
			       gcc_jit_param_as_rvalue (obj));	\
    stack++;							\
  } while (0)

#define TOS (*(stack - 1))

#define DISCARD(n) (stack -= (n))

#define POP0

#define POP1							\
  do {								\
    stack--;							\
    CHECK_STACK;						\
    args[0] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
  } while (0)

#define POP2							\
  do {								\
    stack--;							\
    CHECK_STACK;						\
    args[1] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
    stack--;							\
    args[0] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
  } while (0)

#define POP3							\
  do {								\
    stack--;							\
    CHECK_STACK;						\
    args[2] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
    stack--;							\
    args[1] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
    stack--;							\
    args[0] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);	\
  } while (0)

/* Fetch the next byte from the bytecode stream.  */

#define FETCH (bytestr_data[pc++])

/* Fetch two bytes from the bytecode stream and make a 16-bit number
   out of them.	 */

#define FETCH2 (op = FETCH, op + (FETCH << 8))

#define STR(s) #s

/* With most of the ops we need to do the same stuff so this macros are meant
   to save some typing.	 */

#define CASE(op)				\
  case op :					\
  emit_comment (STR(op))

/* Pop from the meta-stack, emit the call and push the result */

#define EMIT_CALL_N(name, nargs)					\
  do {									\
    POP##nargs;								\
    res = emit_call ((name), comp.lisp_obj_type, (nargs), args);	\
    PUSH_RVAL (res);							\
  } while (0)

/* Generate appropriate case and emit call to function. */

#define CASE_CALL_N(name, nargs)					\
  CASE (B##name);							\
  EMIT_CALL_N (STR(F##name), nargs);					\
  break

/*
  Emit calls to functions with prototype (ptrdiff_t nargs, Lisp_Object *args).
  This is done by passing a reference to the first obj involved on the stack.
*/

#define EMIT_CALL_N_REF(name, nargs)				\
  do {								\
    DISCARD (nargs);						\
    res = emit_call_n_ref ((name), (nargs), stack->gcc_lval);	\
    PUSH_RVAL (res);						\
  } while (0)

#define EMIT_ARITHCOMPARE(comparison)					\
  do {									\
    POP2;								\
    args[2] = gcc_jit_context_new_rvalue_from_int (comp.ctxt,		\
						   comp.int_type,	\
						   (comparison));	\
    res = emit_call ("arithcompare", comp.lisp_obj_type, 3, args);	\
    PUSH_RVAL (res);							\
  } while (0)


#define SAFE_ALLOCA_BLOCK(ptr, func, name)			\
do {								\
  (ptr) = SAFE_ALLOCA (sizeof (basic_block_t));			\
  (ptr)->gcc_bb = gcc_jit_function_new_block ((func), (name));	\
  (ptr)->terminated = false;					\
  (ptr)->top = NULL;						\
 } while (0)

#define DECL_AND_SAFE_ALLOCA_BLOCK(name, func)	\
  basic_block_t *(name);			\
  SAFE_ALLOCA_BLOCK ((name), (func), STR(name))

/* Element of the meta stack.  */
typedef struct {
  gcc_jit_lvalue *gcc_lval;
  enum Lisp_Type type; /* -1 if not set.  */
  Lisp_Object constant; /* This is used for constant propagation.   */
  bool const_set;
} stack_el_t;

typedef struct {
  gcc_jit_block *gcc_bb;
  /* When non zero indicates a stack pointer restart.  */
  stack_el_t *top;
  bool terminated;
} basic_block_t;

/* The compiler context	 */

typedef struct {
  gcc_jit_context *ctxt;
  gcc_jit_type *void_type;
  gcc_jit_type *bool_type;
  gcc_jit_type *char_type;
  gcc_jit_type *int_type;
  gcc_jit_type *unsigned_type;
  gcc_jit_type *long_type;
  gcc_jit_type *unsigned_long_type;
  gcc_jit_type *long_long_type;
  gcc_jit_type *unsigned_long_long_type;
  gcc_jit_type *emacs_int_type;
  gcc_jit_type *void_ptr_type;
  gcc_jit_type *char_ptr_type;
  gcc_jit_type *ptrdiff_type;
  gcc_jit_type *uintptr_type;
  gcc_jit_type *lisp_obj_type;
  gcc_jit_type *lisp_obj_ptr_type;
  gcc_jit_field *lisp_obj_as_ptr;
  gcc_jit_field *lisp_obj_as_num;
  /* struct Lisp_Cons */
  gcc_jit_struct *lisp_cons_s;
  gcc_jit_field *lisp_cons_u;
  gcc_jit_field *lisp_cons_u_s;
  gcc_jit_field *lisp_cons_u_s_car;
  gcc_jit_field *lisp_cons_u_s_u;
  gcc_jit_field *lisp_cons_u_s_u_cdr;
  gcc_jit_type *lisp_cons_type;
  gcc_jit_type *lisp_cons_ptr_type;
  /* struct jmp_buf.  */
  gcc_jit_struct *jmp_buf_s;
  /* struct handler.  */
  gcc_jit_struct *handler_s;
  gcc_jit_field *handler_jmp_field;
  gcc_jit_field *handler_val_field;
  gcc_jit_field *handler_next_field;
  gcc_jit_type *handler_ptr_type;
  /* struct thread_state.  */
  gcc_jit_struct *thread_state_s;
  gcc_jit_field *m_handlerlist;
  gcc_jit_type *thread_state_ptr_type;
  gcc_jit_rvalue *current_thread;
  /* other globals */
  gcc_jit_rvalue *pure;
  /* libgccjit has really limited support for casting therefore this union will
     be used for the scope.  */
  gcc_jit_type *cast_union_type;
  gcc_jit_field *cast_union_as_ll;
  gcc_jit_field *cast_union_as_ull;
  gcc_jit_field *cast_union_as_l;
  gcc_jit_field *cast_union_as_ul;
  gcc_jit_field *cast_union_as_u;
  gcc_jit_field *cast_union_as_i;
  gcc_jit_field *cast_union_as_b;
  gcc_jit_field *cast_union_as_c_p;
  gcc_jit_field *cast_union_as_v_p;
  gcc_jit_field *cast_union_as_lisp_cons_ptr;
  gcc_jit_field *cast_union_as_lisp_obj;
  gcc_jit_function *func; /* Current function being compiled  */
  gcc_jit_rvalue *most_positive_fixnum;
  gcc_jit_rvalue *most_negative_fixnum;
  gcc_jit_rvalue *one;
  gcc_jit_rvalue *inttypebits;
  gcc_jit_rvalue *lisp_int0;
  gcc_jit_function *pseudovectorp;
  gcc_jit_function *bool_to_lisp_obj;
  gcc_jit_function *car;
  gcc_jit_function *cdr;
  gcc_jit_function *setcar;
  gcc_jit_function *setcdr;
  gcc_jit_function *check_type;
  gcc_jit_function *check_impure;
  basic_block_t *block; /* Current basic block	 */
  Lisp_Object func_hash; /* f_name -> gcc_func	*/
} comp_t;

static comp_t comp;

FILE *logfile = NULL;

/* The result of one function compilation.  */

typedef struct {
  gcc_jit_result *gcc_res;
  short min_args, max_args;
} comp_f_res_t;

void emacs_native_compile (const char *lisp_f_name, const char *c_f_name,
			   Lisp_Object func, int opt_level, bool dump_asm);

static char * ATTRIBUTE_FORMAT_PRINTF (1, 2)
format_string (const char *format, ...)
{
  static char scratch_area[512];
  va_list va;
  va_start (va, format);
  int res = vsnprintf (scratch_area, sizeof (scratch_area), format, va);
  if (res >= sizeof (scratch_area))
    error ("Truncating string");
  va_end (va);
  return scratch_area;
}

static void
bcall0 (Lisp_Object f)
{
  Ffuncall (1, &f);
}

/* Pop form the main evaluation stack and place the elements in args in reversed
   order.  */

INLINE static void
pop (unsigned n, stack_el_t **stack_ref, gcc_jit_rvalue *args[])
{
  eassert (n <= MAX_POP); /* FIXME?  */
  stack_el_t *stack = *stack_ref;

  while (n--)
    {
      stack--;
      args[n] = gcc_jit_lvalue_as_rvalue (stack->gcc_lval);
    }

  *stack_ref = stack;
}

INLINE static gcc_jit_field *
type_to_cast_field (gcc_jit_type *type)
{
  gcc_jit_field *field;

  if (type == comp.long_long_type)
    field = comp.cast_union_as_ll;
  else if (type == comp.unsigned_long_long_type)
    field = comp.cast_union_as_ull;
  else if (type == comp.long_type)
    field = comp.cast_union_as_l;
  else if (type == comp.unsigned_long_type)
    field = comp.cast_union_as_ul;
  else if (type == comp.unsigned_type)
    field = comp.cast_union_as_u;
  else if (type == comp.int_type)
    field = comp.cast_union_as_i;
  else if (type == comp.bool_type)
    field = comp.cast_union_as_b;
  else if (type == comp.void_ptr_type)
    field = comp.cast_union_as_v_p;
  else if (type == comp.char_ptr_type)
    field = comp.cast_union_as_c_p;
  else if (type == comp.lisp_cons_ptr_type)
    field = comp.cast_union_as_lisp_cons_ptr;
  else if (type == comp.lisp_obj_type)
    field = comp.cast_union_as_lisp_obj;
  else
    error ("unsupported cast\n");

  return field;
}

INLINE static void
emit_comment (const char *str)
{
  if (COMP_DEBUG)
    gcc_jit_block_add_comment (comp.block->gcc_bb,
			       NULL,
			       str);
}


/* Assignments to the meta-stack slots should be emitted usign this to always */
/* reset annotation fields.   */

static void
emit_assign_to_stack_slot(basic_block_t *block, stack_el_t *slot,
			  gcc_jit_rvalue *val)
{
  gcc_jit_block_add_assignment (block->gcc_bb,
				NULL,
				slot->gcc_lval,
				val);
  slot->type = -1;
  slot->const_set = false;
}


static gcc_jit_function *
emit_func_declare (const char *f_name, gcc_jit_type *ret_type,
		   unsigned nargs, gcc_jit_rvalue **args,
		   enum	 gcc_jit_function_kind kind, bool reusable)
{
  gcc_jit_param *param[4];
  gcc_jit_type *type[4];

  /* If args are passed types are extracted from that otherwise assume params */
  /* are all lisp objs.	 */
  if (args)
    for (int i = 0; i < nargs; i++)
      type[i] = gcc_jit_rvalue_get_type (args[i]);
  else
    for (int i = 0; i < nargs; i++)
      type[i] = comp.lisp_obj_type;

  switch (nargs) {
  case 4:
    param[3] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[3],
					 "c");
    /* Fall through */
    FALLTHROUGH;
  case 3:
    param[2] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[2],
					 "c");
    /* Fall through */
    FALLTHROUGH;
  case 2:
    param[1] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[1],
					 "b");
    /* Fall through */
    FALLTHROUGH;
  case 1:
    param[0] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[0],
					 "a");
    /* Fall through */
    FALLTHROUGH;
  case 0:
    break;
  default:
    /* Argnum not supported  */
    eassert (0);
  }

  gcc_jit_function *func =
    gcc_jit_context_new_function(comp.ctxt, NULL,
				 kind,
				 ret_type,
				 f_name,
				 nargs,
				 param,
				 0);

  if (reusable)
    {
      Lisp_Object value;
      Lisp_Object key = make_string (f_name, strlen (f_name));
      value = make_pointer_integer (XPL (func));

      EMACS_UINT hash = 0;
      struct Lisp_Hash_Table *ht = XHASH_TABLE (comp.func_hash);
      ptrdiff_t i = hash_lookup (ht, key, &hash);
      /* Don't want to declare the same function two times */
      eassert (i == -1);
      hash_put (ht, key, value, hash);
    }

  return func;
}

static gcc_jit_rvalue *
emit_call (const char *f_name, gcc_jit_type *ret_type, unsigned nargs,
	   gcc_jit_rvalue **args)
{
  Lisp_Object key = make_string (f_name, strlen (f_name));
  EMACS_UINT hash = 0;
  struct Lisp_Hash_Table *ht = XHASH_TABLE (comp.func_hash);
  ptrdiff_t i = hash_lookup (ht, key, &hash);

  if (i == -1)
    {
      emit_func_declare(f_name, ret_type, nargs, args, GCC_JIT_FUNCTION_IMPORTED,
			true);
      i = hash_lookup (ht, key, &hash);
      eassert (i != -1);
    }

  Lisp_Object value = HASH_VALUE (ht, hash_lookup (ht, key, &hash));
  gcc_jit_function *func = (gcc_jit_function *) XFIXNUMPTR (value);

  return gcc_jit_context_new_call(comp.ctxt,
				  NULL,
				  func,
				  nargs,
				  args);
}

/* Close current basic block emitting a conditional.  */

INLINE static void
emit_cond_jump (gcc_jit_rvalue *test,
		basic_block_t *then_target, basic_block_t *else_target)
{
  if (gcc_jit_rvalue_get_type (test) == comp.bool_type)
    gcc_jit_block_end_with_conditional (comp.block->gcc_bb,
				      NULL,
				      test,
				      then_target->gcc_bb,
				      else_target->gcc_bb);
  else
    /* In case test is not bool we do a logical negation to obtain a bool as
       result.  */
    gcc_jit_block_end_with_conditional (
      comp.block->gcc_bb,
      NULL,
      gcc_jit_context_new_unary_op (comp.ctxt,
				    NULL,
				    GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
				    comp.bool_type,
				    test),
      else_target->gcc_bb,
      then_target->gcc_bb);

  comp.block->terminated = true;
}

/* Close current basic block emitting a comparison between two rval.  */

static gcc_jit_rvalue *
emit_comparison_jump (enum gcc_jit_comparison op,
		     gcc_jit_rvalue *a, gcc_jit_rvalue *b,
		     basic_block_t *then_target, basic_block_t *else_target)
{
  gcc_jit_rvalue *test = gcc_jit_context_new_comparison (comp.ctxt,
							 NULL,
							 op,
							 a, b);

  emit_cond_jump (test, then_target, else_target);

  return test;
}

static gcc_jit_rvalue *
emit_cast (gcc_jit_type *new_type, gcc_jit_rvalue *obj)
{
  static unsigned i;

  gcc_jit_field *orig_field =
    type_to_cast_field (gcc_jit_rvalue_get_type (obj));
  gcc_jit_field *dest_field = type_to_cast_field (new_type);

  gcc_jit_lvalue *tmp_u =
    gcc_jit_function_new_local (comp.func,
				NULL,
				comp.cast_union_type,
				format_string ("union_cast_%u", i++));
  gcc_jit_block_add_assignment (comp.block->gcc_bb,
				NULL,
				gcc_jit_lvalue_access_field (tmp_u,
							     NULL,
							     orig_field),
				obj);

  return gcc_jit_rvalue_access_field ( gcc_jit_lvalue_as_rvalue (tmp_u),
				       NULL,
				       dest_field);
}

INLINE static gcc_jit_rvalue *
emit_XLI (gcc_jit_rvalue *obj)
{
  emit_comment ("XLI");

  return gcc_jit_rvalue_access_field (obj,
				      NULL,
				      comp.lisp_obj_as_num);
}

INLINE static gcc_jit_lvalue *
emit_lval_XLI (gcc_jit_lvalue *obj)
{
  emit_comment ("lval_XLI");

  return gcc_jit_lvalue_access_field (obj,
				      NULL,
				      comp.lisp_obj_as_num);
}

INLINE static gcc_jit_rvalue *
emit_XLP (gcc_jit_rvalue *obj)
{
  emit_comment ("XLP");

  return gcc_jit_rvalue_access_field (obj,
				      NULL,
				      comp.lisp_obj_as_ptr);
}

INLINE static gcc_jit_lvalue *
emit_lval_XLP (gcc_jit_lvalue *obj)
{
  emit_comment ("lval_XLP");

  return gcc_jit_lvalue_access_field (obj,
				      NULL,
				      comp.lisp_obj_as_ptr);
}

static gcc_jit_rvalue *
emit_XUNTAG (gcc_jit_rvalue *a, gcc_jit_type *type, unsigned lisp_word_tag)
{
  /* #define XUNTAG(a, type, ctype) ((ctype *)
     ((char *) XLP (a) - LISP_WORD_TAG (type))) */
  emit_comment ("XUNTAG");

  return emit_cast (gcc_jit_type_get_pointer (type),
	   gcc_jit_context_new_binary_op (
	     comp.ctxt,
	     NULL,
	     GCC_JIT_BINARY_OP_MINUS,
	     comp.emacs_int_type,
	     emit_XLI (a),
	     gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						  comp.emacs_int_type,
						  lisp_word_tag)));
}

static gcc_jit_rvalue *
emit_XCONS (gcc_jit_rvalue *a)
{
  emit_comment ("XCONS");

  return emit_XUNTAG (a,
		      gcc_jit_struct_as_type (comp.lisp_cons_s),
		      LISP_WORD_TAG (Lisp_Cons));
}

static gcc_jit_rvalue *
emit_EQ (gcc_jit_rvalue *x, gcc_jit_rvalue *y)
{
  emit_comment ("EQ");

  return gcc_jit_context_new_comparison (
	   comp.ctxt,
	   NULL,
	   GCC_JIT_COMPARISON_EQ,
	   emit_XLI (x),
	   emit_XLI (y));
}

static gcc_jit_rvalue *
emit_TAGGEDP (gcc_jit_rvalue *obj, unsigned tag)
{
   /* (! (((unsigned) (XLI (a) >> (USE_LSB_TAG ? 0 : VALBITS)) \
	- (unsigned) (tag)) \
	& ((1 << GCTYPEBITS) - 1))) */
  emit_comment ("TAGGEDP");

  gcc_jit_rvalue *sh_res =
    gcc_jit_context_new_binary_op (
      comp.ctxt,
      NULL,
      GCC_JIT_BINARY_OP_RSHIFT,
      comp.emacs_int_type,
      emit_XLI (obj),
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   comp.emacs_int_type,
					   (USE_LSB_TAG ? 0 : VALBITS)));

  gcc_jit_rvalue *minus_res =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_MINUS,
				   comp.unsigned_type,
				   emit_cast (comp.unsigned_type, sh_res),
				   gcc_jit_context_new_rvalue_from_int (
				     comp.ctxt,
				     comp.unsigned_type,
				     tag));

  gcc_jit_rvalue *res =
   gcc_jit_context_new_unary_op (
     comp.ctxt,
     NULL,
     GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
     comp.int_type,
     gcc_jit_context_new_binary_op (comp.ctxt,
				    NULL,
				    GCC_JIT_BINARY_OP_BITWISE_AND,
				    comp.unsigned_type,
				    minus_res,
				    gcc_jit_context_new_rvalue_from_int (
				      comp.ctxt,
				      comp.unsigned_type,
				      ((1 << GCTYPEBITS) - 1))));

  return res;
}

static gcc_jit_rvalue *
emit_VECTORLIKEP (gcc_jit_rvalue *obj)
{
  emit_comment ("VECTORLIKEP");

  return emit_TAGGEDP (obj, Lisp_Vectorlike);
}

static gcc_jit_rvalue *
emit_CONSP (gcc_jit_rvalue *obj)
{
  emit_comment ("CONSP");

  return emit_TAGGEDP (obj, Lisp_Cons);
}

static gcc_jit_rvalue *
emit_FLOATP (gcc_jit_rvalue *obj)
{
  emit_comment ("FLOATP");

  return emit_TAGGEDP (obj, Lisp_Float);
}

static gcc_jit_rvalue *
emit_BIGNUMP (gcc_jit_rvalue *obj)
{
  /* PSEUDOVECTORP (x, PVEC_BIGNUM); */
  emit_comment ("BIGNUMP");

  gcc_jit_rvalue *args[2] = {
    obj,
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.int_type,
					 PVEC_BIGNUM) };

  return gcc_jit_context_new_call (comp.ctxt,
				   NULL,
				   comp.pseudovectorp,
				   2,
				   args);
}

static gcc_jit_rvalue *
emit_FIXNUMP (gcc_jit_rvalue *obj)
{
  /* (! (((unsigned) (XLI (x) >> (USE_LSB_TAG ? 0 : FIXNUM_BITS))
	- (unsigned) (Lisp_Int0 >> !USE_LSB_TAG))
	& ((1 << INTTYPEBITS) - 1)))  */
  emit_comment ("FIXNUMP");

  gcc_jit_rvalue *sh_res =
    gcc_jit_context_new_binary_op (
      comp.ctxt,
      NULL,
      GCC_JIT_BINARY_OP_RSHIFT,
      comp.emacs_int_type,
      emit_XLI (obj),
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   comp.emacs_int_type,
					   (USE_LSB_TAG ? 0 : FIXNUM_BITS)));

  gcc_jit_rvalue *minus_res =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_MINUS,
				   comp.unsigned_type,
				   emit_cast (comp.unsigned_type, sh_res),
				   gcc_jit_context_new_rvalue_from_int (
				     comp.ctxt,
				     comp.unsigned_type,
				     (Lisp_Int0 >> !USE_LSB_TAG)));

  gcc_jit_rvalue *res =
   gcc_jit_context_new_unary_op (
     comp.ctxt,
     NULL,
     GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
     comp.int_type,
     gcc_jit_context_new_binary_op (comp.ctxt,
				    NULL,
				    GCC_JIT_BINARY_OP_BITWISE_AND,
				    comp.unsigned_type,
				    minus_res,
				    gcc_jit_context_new_rvalue_from_int (
				      comp.ctxt,
				      comp.unsigned_type,
				      ((1 << INTTYPEBITS) - 1))));

  return res;
}

static gcc_jit_rvalue *
emit_XFIXNUM (gcc_jit_rvalue *obj)
{
  emit_comment ("XFIXNUM");

  return gcc_jit_context_new_binary_op (comp.ctxt,
					NULL,
					GCC_JIT_BINARY_OP_RSHIFT,
					comp.emacs_int_type,
					emit_XLI (obj),
					comp.inttypebits);
}

static gcc_jit_rvalue *
emit_INTEGERP (gcc_jit_rvalue *obj)
{
  emit_comment ("INTEGERP");

  return gcc_jit_context_new_binary_op (comp.ctxt,
					NULL,
					GCC_JIT_BINARY_OP_LOGICAL_OR,
					comp.bool_type,
					emit_cast (comp.bool_type,
						   emit_FIXNUMP (obj)),
					emit_BIGNUMP (obj));
}

static gcc_jit_rvalue *
emit_NUMBERP (gcc_jit_rvalue *obj)
{
  emit_comment ("NUMBERP");

  return gcc_jit_context_new_binary_op (comp.ctxt,
					NULL,
					GCC_JIT_BINARY_OP_LOGICAL_OR,
					comp.bool_type,
					emit_INTEGERP(obj),
					emit_cast (comp.bool_type,
						   emit_FLOATP (obj)));
}

static gcc_jit_rvalue *
emit_make_fixnum (gcc_jit_rvalue *obj)
{
  emit_comment ("make_fixnum");

  gcc_jit_rvalue *tmp =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_LSHIFT,
				   comp.emacs_int_type,
				   obj,
				   comp.inttypebits);

  tmp = gcc_jit_context_new_binary_op (comp.ctxt,
				       NULL,
				       GCC_JIT_BINARY_OP_PLUS,
				       comp.emacs_int_type,
				       tmp,
				       comp.lisp_int0);

  gcc_jit_lvalue *res = gcc_jit_function_new_local (comp.func,
						    NULL,
						    comp.lisp_obj_type,
						    "lisp_obj_fixnum");

  gcc_jit_block_add_assignment (comp.block->gcc_bb,
				NULL,
				emit_lval_XLI (res),
				tmp);

  return gcc_jit_lvalue_as_rvalue (res);
}

/* Construct fill and return a lisp object form a raw pointer.	*/
static gcc_jit_rvalue *
emit_lisp_obj_from_ptr (void *p)
{
  static unsigned i;
  emit_comment ("lisp_obj_from_ptr");

  gcc_jit_lvalue *lisp_obj =
    gcc_jit_function_new_local (comp.func,
				NULL,
				comp.lisp_obj_type,
				format_string ("lisp_obj_from_ptr_%u", i++));
  gcc_jit_rvalue *void_ptr =
    gcc_jit_context_new_rvalue_from_ptr(comp.ctxt,
					comp.void_ptr_type,
					p);

  if (SYMBOLP (p))
    emit_comment (
      format_string ("Symbol %s",
		     (char *) SDATA (SYMBOL_NAME (p))));

  gcc_jit_block_add_assignment (comp.block->gcc_bb,
				NULL,
				emit_lval_XLP (lisp_obj),
				void_ptr);

  return gcc_jit_lvalue_as_rvalue (lisp_obj);
}

static gcc_jit_rvalue *
emit_NILP (gcc_jit_rvalue *x)
{
  emit_comment ("NILP");

  return emit_EQ (x, emit_lisp_obj_from_ptr (Qnil));
}

static gcc_jit_rvalue *
emit_XCAR (gcc_jit_rvalue *c)
{
  emit_comment ("XCAR");

  /* XCONS (c)->u.s.car */
  return
    gcc_jit_rvalue_access_field (
      /* XCONS (c)->u.s */
      gcc_jit_rvalue_access_field (
	/* XCONS (c)->u */
	gcc_jit_lvalue_as_rvalue (
	  gcc_jit_rvalue_dereference_field (
	    emit_XCONS (c),
	    NULL,
	    comp.lisp_cons_u)),
	NULL,
	comp.lisp_cons_u_s),
      NULL,
      comp.lisp_cons_u_s_car);
}

static gcc_jit_lvalue *
emit_lval_XCAR (gcc_jit_rvalue *c)
{
  emit_comment ("lval_XCAR");

  /* XCONS (c)->u.s.car */
  return
    gcc_jit_lvalue_access_field (
      /* XCONS (c)->u.s */
      gcc_jit_lvalue_access_field (
	/* XCONS (c)->u */
	gcc_jit_rvalue_dereference_field (
	  emit_XCONS (c),
	  NULL,
	  comp.lisp_cons_u),
	NULL,
	comp.lisp_cons_u_s),
      NULL,
      comp.lisp_cons_u_s_car);
}

static gcc_jit_rvalue *
emit_XCDR (gcc_jit_rvalue *c)
{
  emit_comment ("XCDR");
  /* XCONS (c)->u.s.u.cdr */
  return
    gcc_jit_rvalue_access_field (
      /* XCONS (c)->u.s.u */
      gcc_jit_rvalue_access_field (
	/* XCONS (c)->u.s */
	gcc_jit_rvalue_access_field (
	  /* XCONS (c)->u */
	  gcc_jit_lvalue_as_rvalue (
	    gcc_jit_rvalue_dereference_field (
	      emit_XCONS (c),
	      NULL,
	      comp.lisp_cons_u)),
	  NULL,
	  comp.lisp_cons_u_s),
	NULL,
	comp.lisp_cons_u_s_u),
      NULL,
      comp.lisp_cons_u_s_u_cdr);
}

static gcc_jit_lvalue *
emit_lval_XCDR (gcc_jit_rvalue *c)
{
  emit_comment ("lval_XCDR");

  /* XCONS (c)->u.s.u.cdr */
  return
    gcc_jit_lvalue_access_field (
      /* XCONS (c)->u.s.u */
      gcc_jit_lvalue_access_field (
	/* XCONS (c)->u.s */
	gcc_jit_lvalue_access_field (
	  /* XCONS (c)->u */
	  gcc_jit_rvalue_dereference_field (
	    emit_XCONS (c),
	    NULL,
	    comp.lisp_cons_u),
	  NULL,
	  comp.lisp_cons_u_s),
	NULL,
	comp.lisp_cons_u_s_u),
      NULL,
      comp.lisp_cons_u_s_u_cdr);
}

static void
emit_CHECK_CONS (gcc_jit_rvalue *x)
{
  emit_comment ("CHECK_CONS");

  gcc_jit_rvalue *args[] =
    { emit_CONSP (x),
      emit_lisp_obj_from_ptr (Qconsp),
      x };

  gcc_jit_block_add_eval (
    comp.block->gcc_bb,
    NULL,
    gcc_jit_context_new_call (comp.ctxt,
			      NULL,
			      comp.check_type,
			      3,
			      args));
}

static gcc_jit_rvalue *
emit_car_addr (gcc_jit_rvalue *c)
{
  emit_comment ("car_addr");

  return gcc_jit_lvalue_get_address (emit_lval_XCAR (c), NULL);
}

static gcc_jit_rvalue *
emit_cdr_addr (gcc_jit_rvalue *c)
{
  emit_comment ("cdr_addr");

  return gcc_jit_lvalue_get_address (emit_lval_XCDR (c), NULL);
}

static void
emit_XSETCAR (gcc_jit_rvalue *c, gcc_jit_rvalue *n)
{
  emit_comment ("XSETCAR");

  gcc_jit_block_add_assignment(
    comp.block->gcc_bb,
    NULL,
    gcc_jit_rvalue_dereference (
      emit_car_addr (c),
      NULL),
    n);
}

static void
emit_XSETCDR (gcc_jit_rvalue *c, gcc_jit_rvalue *n)
{
  emit_comment ("XSETCDR");

  gcc_jit_block_add_assignment(
    comp.block->gcc_bb,
    NULL,
    gcc_jit_rvalue_dereference (
      emit_cdr_addr (c),
      NULL),
    n);
}

static gcc_jit_rvalue *
emit_PURE_P (gcc_jit_rvalue *ptr)
{

  emit_comment ("PURE_P");

  return
    gcc_jit_context_new_comparison (
      comp.ctxt,
      NULL,
      GCC_JIT_COMPARISON_LE,
      gcc_jit_context_new_binary_op (
	comp.ctxt,
	NULL,
	GCC_JIT_BINARY_OP_MINUS,
	comp.uintptr_type,
	emit_cast (comp.uintptr_type, ptr),
	emit_cast (comp.uintptr_type, comp.pure)),
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   comp.uintptr_type,
					   PURESIZE));
}

static gcc_jit_rvalue *
emit_call_n_ref (const char *f_name, unsigned nargs,
		 gcc_jit_lvalue *base_arg)
{
  gcc_jit_rvalue *args[] =
    { gcc_jit_context_new_rvalue_from_int(comp.ctxt,
					  comp.ptrdiff_type,
					  nargs),
      gcc_jit_lvalue_get_address (base_arg, NULL) };
  return emit_call (f_name, comp.lisp_obj_type, 2, args);
}

/* struct Lisp_Cons definition.  */

static void
define_lisp_cons (void)
{
  /*
    union cdr_u
    {
      Lisp_Object cdr;
      struct Lisp_Cons *chain;
    };

    struct cons_s
    {
      Lisp_Object car;
      union cdr_u u;
    };

    union cons_u
    {
      struct cons_s s;
      char align_pad[sizeof (struct Lisp_Cons)];
    };

    struct Lisp_Cons
    {
      union cons_u u;
    };
  */

  comp.lisp_cons_s =
    gcc_jit_context_new_opaque_struct (comp.ctxt,
				       NULL,
				       "comp_Lisp_Cons");
  comp.lisp_cons_type =
    gcc_jit_struct_as_type (comp.lisp_cons_s);
  comp.lisp_cons_ptr_type =
    gcc_jit_type_get_pointer (comp.lisp_cons_type);

  comp.lisp_cons_u_s_u_cdr =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.lisp_obj_type,
			       "cdr");

  gcc_jit_field *cdr_u_fields[] =
    { comp.lisp_cons_u_s_u_cdr,
      gcc_jit_context_new_field (comp.ctxt,
				 NULL,
				 comp.lisp_cons_ptr_type,
				 "chain") };

  gcc_jit_type *cdr_u =
    gcc_jit_context_new_union_type (comp.ctxt,
				    NULL,
				    "comp_cdr_u",
				    sizeof (cdr_u_fields)
				    / sizeof (*cdr_u_fields),
				    cdr_u_fields);

  comp.lisp_cons_u_s_car = gcc_jit_context_new_field (comp.ctxt,
					    NULL,
					    comp.lisp_obj_type,
					    "car");
  comp.lisp_cons_u_s_u = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    cdr_u,
						    "u");
  gcc_jit_field *cons_s_fields[] =
    { comp.lisp_cons_u_s_car,
      comp.lisp_cons_u_s_u };

  gcc_jit_struct *cons_s =
    gcc_jit_context_new_struct_type (comp.ctxt,
				     NULL,
				     "comp_cons_s",
				     sizeof (cons_s_fields)
				     / sizeof (*cons_s_fields),
				     cons_s_fields);

  comp.lisp_cons_u_s = gcc_jit_context_new_field (comp.ctxt,
				 NULL,
				 gcc_jit_struct_as_type (cons_s),
				 "s");

  gcc_jit_field *cons_u_fields[] =
    { comp.lisp_cons_u_s,
      gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (comp.ctxt,
					NULL,
					comp.char_type,
					sizeof (struct Lisp_Cons)),
	"align_pad") };

  gcc_jit_type *lisp_cons_u_type =
    gcc_jit_context_new_union_type (comp.ctxt,
				    NULL,
				    "comp_cons_u",
				    sizeof (cons_u_fields)
				    / sizeof (*cons_u_fields),
				    cons_u_fields);

  comp.lisp_cons_u =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       lisp_cons_u_type,
			       "u");
  gcc_jit_struct_set_fields (comp.lisp_cons_s,
			     NULL, 1, &comp.lisp_cons_u);

}

/* opaque jmp_buf definition.  */

static void
define_jmp_buf (void)
{
  gcc_jit_field *field =
    gcc_jit_context_new_field (
      comp.ctxt,
      NULL,
      gcc_jit_context_new_array_type (comp.ctxt,
				      NULL,
				      comp.char_type,
				      sizeof (jmp_buf)),
      "stuff");
  comp.jmp_buf_s =
    gcc_jit_context_new_struct_type (comp.ctxt,
				     NULL,
				     "comp_jmp_buf",
				     1, &field);
}

/* struct handler definition  */

static void
define_handler_struct (void)
{
  comp.handler_s =
    gcc_jit_context_new_opaque_struct (comp.ctxt, NULL, "comp_handler");
  comp.handler_ptr_type =
    gcc_jit_type_get_pointer (gcc_jit_struct_as_type (comp.handler_s));

  comp.handler_jmp_field = gcc_jit_context_new_field (comp.ctxt,
						      NULL,
						      gcc_jit_struct_as_type (
							comp.jmp_buf_s),
						      "jmp");
  comp.handler_val_field = gcc_jit_context_new_field (comp.ctxt,
						      NULL,
						      comp.lisp_obj_type,
						      "val");
  comp.handler_next_field = gcc_jit_context_new_field (comp.ctxt,
						       NULL,
						       comp.handler_ptr_type,
						       "next");
  gcc_jit_field *fields[] =
    { gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (comp.ctxt,
					NULL,
					comp.char_type,
					offsetof (struct handler, val)),
	"pad0"),
      comp.handler_val_field,
      comp.handler_next_field,
      gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (comp.ctxt,
					NULL,
					comp.char_type,
					offsetof (struct handler, jmp)
					- offsetof (struct handler, next)
					- sizeof (((struct handler *) 0)->next)),
	"pad1"),
      comp.handler_jmp_field,
      gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (comp.ctxt,
					NULL,
					comp.char_type,
					sizeof (struct handler)
					- offsetof (struct handler, jmp)
					- sizeof (((struct handler *) 0)->jmp)),
	"pad2") };
  gcc_jit_struct_set_fields (comp.handler_s,
			     NULL,
			     sizeof (fields) / sizeof (*fields),
			     fields);

}

static void
define_thread_state_struct (void)
{
  /* Partially opaque definition for `thread_state'.
     Because we need to access just m_handlerlist hopefully this is requires
     less manutention then the full deifnition.	 */

  comp.m_handlerlist = gcc_jit_context_new_field (comp.ctxt,
						  NULL,
						  comp.handler_ptr_type,
						  "m_handlerlist");
  gcc_jit_field *fields[] =
    { gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (comp.ctxt,
					NULL,
					comp.char_type,
					offsetof (struct thread_state,
						  m_handlerlist)),
	"pad0"),
      comp.m_handlerlist,
      gcc_jit_context_new_field (
	comp.ctxt,
	NULL,
	gcc_jit_context_new_array_type (
	  comp.ctxt,
	  NULL,
	  comp.char_type,
	  sizeof (struct thread_state)
	  - offsetof (struct thread_state,
		      m_handlerlist)
	  - sizeof (((struct thread_state *) 0)->m_handlerlist)),
	"pad1") };

  comp.thread_state_s =
    gcc_jit_context_new_struct_type (comp.ctxt,
				     NULL,
				     "comp_thread_state",
				     sizeof (fields) / sizeof (*fields),
				     fields);
  comp.thread_state_ptr_type =
    gcc_jit_type_get_pointer (gcc_jit_struct_as_type (comp.thread_state_s));
}

static void
define_cast_union (void)
{

  comp.cast_union_as_ll =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.long_long_type,
			       "ll");
  comp.cast_union_as_ull =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.unsigned_long_long_type,
			       "ull");
  comp.cast_union_as_l =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.long_type,
			       "l");
  comp.cast_union_as_ul =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.unsigned_long_type,
			       "ul");
  comp.cast_union_as_u =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.unsigned_type,
			       "u");
  comp.cast_union_as_i =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.int_type,
			       "i");
  comp.cast_union_as_b =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.bool_type,
			       "b");
  comp.cast_union_as_c_p =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.char_ptr_type,
			       "c_p");
  comp.cast_union_as_v_p =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.void_ptr_type,
			       "v_p");
  comp.cast_union_as_lisp_cons_ptr =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.lisp_cons_ptr_type,
			       "cons_ptr");
  comp.cast_union_as_lisp_obj =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.lisp_obj_type,
			       "lisp_obj");

  gcc_jit_field *cast_union_fields[] =
    { comp.cast_union_as_ll,
      comp.cast_union_as_ull,
      comp.cast_union_as_l,
      comp.cast_union_as_ul,
      comp.cast_union_as_u,
      comp.cast_union_as_i,
      comp.cast_union_as_b,
      comp.cast_union_as_c_p,
      comp.cast_union_as_v_p,
      comp.cast_union_as_lisp_cons_ptr,
      comp.cast_union_as_lisp_obj};
  comp.cast_union_type =
    gcc_jit_context_new_union_type (comp.ctxt,
				    NULL,
				    "cast_union",
				    sizeof (cast_union_fields)
				    / sizeof (*cast_union_fields),
				    cast_union_fields);
}

static void
define_CHECK_TYPE (void)
{
  USE_SAFE_ALLOCA;
  gcc_jit_param *param[] =
    { gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.int_type,
				 "ok"),
      gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.lisp_obj_type,
				 "predicate"),
      gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.lisp_obj_type,
				 "x") };
  comp.check_type =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.void_type,
				  "CHECK_TYPE",
				  3,
				  param,
				  0);
  gcc_jit_rvalue *ok = gcc_jit_param_as_rvalue (param[0]);
  gcc_jit_rvalue *predicate = gcc_jit_param_as_rvalue (param[1]);
  gcc_jit_rvalue *x = gcc_jit_param_as_rvalue (param[2]);

  DECL_AND_SAFE_ALLOCA_BLOCK (init_block, comp.check_type);
  DECL_AND_SAFE_ALLOCA_BLOCK (ok_block, comp.check_type);
  DECL_AND_SAFE_ALLOCA_BLOCK (not_ok_block, comp.check_type);

  comp.block = init_block;
  comp.func = comp.check_type;

  emit_cond_jump (ok, ok_block, not_ok_block);

  gcc_jit_block_end_with_void_return (ok_block->gcc_bb, NULL);

  comp.block = not_ok_block;

  gcc_jit_rvalue *wrong_type_args[] = { predicate, x };

  gcc_jit_block_add_eval (comp.block->gcc_bb,
			  NULL,
			  emit_call ("wrong_type_argument",
				     comp.lisp_obj_type, 2, wrong_type_args));

  gcc_jit_block_end_with_void_return (not_ok_block->gcc_bb, NULL);

  SAFE_FREE ();
}


/* Declare a substitute for CAR as always inlined function.  */

static void
define_CAR_CDR (void)
{
  USE_SAFE_ALLOCA;

  gcc_jit_param *car_param =
	gcc_jit_context_new_param (comp.ctxt,
				   NULL,
				   comp.lisp_obj_type,
				   "c");
  comp.car =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.lisp_obj_type,
				  "CAR",
				  1,
				  &car_param,
				  0);
  gcc_jit_param *cdr_param =
	gcc_jit_context_new_param (comp.ctxt,
				   NULL,
				   comp.lisp_obj_type,
				   "c");
  comp.cdr =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.lisp_obj_type,
				  "CDR",
				  1,
				  &cdr_param,
				  0);

  gcc_jit_function *f = comp.car;
  gcc_jit_param *param = car_param;

  for (int i = 0; i < 2; i++)
    {
      gcc_jit_rvalue *c = gcc_jit_param_as_rvalue (param);
      DECL_AND_SAFE_ALLOCA_BLOCK (init_block, f);
      DECL_AND_SAFE_ALLOCA_BLOCK (is_cons_b, f);
      DECL_AND_SAFE_ALLOCA_BLOCK (not_a_cons_b, f);

      comp.block = init_block;
      comp.func = f;

      emit_cond_jump (emit_CONSP (c), is_cons_b, not_a_cons_b);

      comp.block = is_cons_b;

      if (f == comp.car)
	gcc_jit_block_end_with_return (comp.block->gcc_bb,
				       NULL,
				       emit_XCAR (c));
      else
	gcc_jit_block_end_with_return (comp.block->gcc_bb,
				       NULL,
				       emit_XCDR (c));

      comp.block = not_a_cons_b;

      DECL_AND_SAFE_ALLOCA_BLOCK (is_nil_b, f);
      DECL_AND_SAFE_ALLOCA_BLOCK (not_nil_b, f);

      emit_cond_jump (emit_NILP (c), is_nil_b, not_nil_b);

      comp.block = is_nil_b;
      gcc_jit_block_end_with_return (comp.block->gcc_bb,
				     NULL,
				     emit_lisp_obj_from_ptr (Qnil));

      comp.block = not_nil_b;
      gcc_jit_rvalue *wrong_type_args[] =
	{ emit_lisp_obj_from_ptr (Qlistp), c };

      gcc_jit_block_add_eval (comp.block->gcc_bb,
			      NULL,
			      emit_call ("wrong_type_argument",
					 comp.lisp_obj_type, 2, wrong_type_args));
      gcc_jit_block_end_with_return (comp.block->gcc_bb,
				     NULL,
				     emit_lisp_obj_from_ptr (Qnil));
      f = comp.cdr;
      param = cdr_param;
    }

  SAFE_FREE ();
}

static void
define_setcar_setcdr (void)
{
  USE_SAFE_ALLOCA;

  char const *f_name[] = {"setcar", "setcdr"};
  char const *par_name[] = {"new_car", "new_cdr"};

  for (int i = 0; i < 2; i++)
    {
      gcc_jit_param *cell =
	gcc_jit_context_new_param (comp.ctxt,
				   NULL,
				   comp.lisp_obj_type,
				   "cell");
      gcc_jit_param *new_el =
	gcc_jit_context_new_param (comp.ctxt,
				   NULL,
				   comp.lisp_obj_type,
				   par_name[i]);

      gcc_jit_param *param[] = { cell, new_el };

      gcc_jit_function **f_ref = !i ? &comp.setcar : &comp.setcdr;
      *f_ref = gcc_jit_context_new_function (comp.ctxt, NULL,
					     GCC_JIT_FUNCTION_ALWAYS_INLINE,
					     comp.lisp_obj_type,
					     f_name[i],
					     2,
					     param,
					     0);
      DECL_AND_SAFE_ALLOCA_BLOCK (init_block, *f_ref);
      comp.func = *f_ref;
      comp.block = init_block;

      /* CHECK_CONS (cell); */
      emit_CHECK_CONS (gcc_jit_param_as_rvalue (cell));

      /* CHECK_IMPURE (cell, XCONS (cell)); */
      gcc_jit_rvalue *args[] =
	{ gcc_jit_param_as_rvalue (cell),
	  emit_XCONS (gcc_jit_param_as_rvalue (cell)) };

      gcc_jit_block_add_eval (
			      init_block->gcc_bb,
			      NULL,
			      gcc_jit_context_new_call (comp.ctxt,
							NULL,
							comp.check_impure,
							2,
							args));

      /* XSETCDR (cell, newel); */
      if (!i)
	emit_XSETCAR (gcc_jit_param_as_rvalue (cell),
		      gcc_jit_param_as_rvalue (new_el));
      else
	emit_XSETCDR (gcc_jit_param_as_rvalue (cell),
		      gcc_jit_param_as_rvalue (new_el));

      /* return newel; */
      gcc_jit_block_end_with_return (init_block->gcc_bb,
				     NULL,
				     gcc_jit_param_as_rvalue (new_el));
    }
  SAFE_FREE ();
}

/* Declare a substitute for PSEUDOVECTORP as always inlined function.  */

static void
define_PSEUDOVECTORP (void)
{
  USE_SAFE_ALLOCA;

  gcc_jit_param *param[] =
    { gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.lisp_obj_type,
				 "a"),
      gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.int_type,
				 "code") };

  comp.pseudovectorp =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.bool_type,
				  "PSEUDOVECTORP",
				  2,
				  param,
				  0);

  DECL_AND_SAFE_ALLOCA_BLOCK (init_block, comp.pseudovectorp);
  DECL_AND_SAFE_ALLOCA_BLOCK (ret_false_b, comp.pseudovectorp);
  DECL_AND_SAFE_ALLOCA_BLOCK (call_pseudovector_typep_b, comp.pseudovectorp);

  comp.block = init_block;
  comp.func = comp.pseudovectorp;

  emit_cond_jump (emit_VECTORLIKEP (gcc_jit_param_as_rvalue (param[0])),
		  call_pseudovector_typep_b,
		  ret_false_b);

  comp.block = ret_false_b;
  gcc_jit_block_end_with_return (ret_false_b->gcc_bb
				 ,
				 NULL,
				 gcc_jit_context_new_rvalue_from_int(
				   comp.ctxt,
				   comp.bool_type,
				   false));

  gcc_jit_rvalue *args[2] =
    { gcc_jit_param_as_rvalue (param[0]),
      gcc_jit_param_as_rvalue (param[1]) };
  comp.block = call_pseudovector_typep_b;
  /* FIXME use XUNTAG now that's available.  */
  gcc_jit_block_end_with_return (call_pseudovector_typep_b->gcc_bb
				 ,
				 NULL,
				 emit_call ("helper_PSEUDOVECTOR_TYPEP_XUNTAG",
					    comp.bool_type,
					    2,
					    args));
  SAFE_FREE ();
}

static void
define_CHECK_IMPURE (void)
{
  USE_SAFE_ALLOCA;

  gcc_jit_param *param[] =
    { gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.lisp_obj_type,
				 "obj"),
      gcc_jit_context_new_param (comp.ctxt,
				 NULL,
				 comp.void_ptr_type,
				 "ptr") };
  comp.check_impure =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.void_type,
				  "CHECK_IMPURE",
				  2,
				  param,
				  0);

    DECL_AND_SAFE_ALLOCA_BLOCK (init_block, comp.check_impure);
    DECL_AND_SAFE_ALLOCA_BLOCK (err_block, comp.check_impure);
    DECL_AND_SAFE_ALLOCA_BLOCK (ok_block, comp.check_impure);

    comp.block = init_block;
    comp.func = comp.check_impure;

    emit_cond_jump (emit_PURE_P (gcc_jit_param_as_rvalue (param[0])), /* FIXME */
		    err_block,
		    ok_block);
    gcc_jit_block_end_with_void_return (ok_block->gcc_bb, NULL);

    gcc_jit_rvalue *pure_write_error_arg =
      gcc_jit_param_as_rvalue (param[0]);

    comp.block = err_block;
    gcc_jit_block_add_eval (comp.block->gcc_bb,
			    NULL,
			    emit_call ("pure_write_error",
				       comp.void_type, 1,
				       &pure_write_error_arg));

    gcc_jit_block_end_with_void_return (err_block->gcc_bb, NULL);

    SAFE_FREE ();}

/* Declare a function to convert boolean into t or nil */

static void
define_bool_to_lisp_obj (void)
{
  USE_SAFE_ALLOCA;

  /* x ? Qt : Qnil */
  gcc_jit_param *param = gcc_jit_context_new_param (comp.ctxt,
						    NULL,
						    comp.bool_type,
						    "x");
  comp.bool_to_lisp_obj =
    gcc_jit_context_new_function (comp.ctxt, NULL,
				  GCC_JIT_FUNCTION_ALWAYS_INLINE,
				  comp.lisp_obj_type,
				  "bool_to_lisp_obj",
				  1,
				  &param,
				  0);
  DECL_AND_SAFE_ALLOCA_BLOCK (init_block, comp.bool_to_lisp_obj);
  DECL_AND_SAFE_ALLOCA_BLOCK (ret_t_block, comp.bool_to_lisp_obj);
  DECL_AND_SAFE_ALLOCA_BLOCK (ret_nil_block, comp.bool_to_lisp_obj);
  comp.block = init_block;
  comp.func = comp.bool_to_lisp_obj;

  emit_cond_jump (gcc_jit_param_as_rvalue (param),
		  ret_t_block,
		  ret_nil_block);

  comp.block = ret_t_block;
  gcc_jit_block_end_with_return (ret_t_block->gcc_bb,
				 NULL,
				 emit_lisp_obj_from_ptr (Qt));

  comp.block = ret_nil_block;
  gcc_jit_block_end_with_return (ret_nil_block->gcc_bb,
				 NULL,
				 emit_lisp_obj_from_ptr (Qnil));

  SAFE_FREE ();
}

static int
ucmp(const void *a, const void *b)
{
#define _I(x) *(const int*)x
  return _I(a) < _I(b) ? -1 : _I(a) > _I(b);
#undef _I
}

/* Compute and initialize all basic blocks.  */
static basic_block_t *
compute_blocks (ptrdiff_t bytestr_length, unsigned char *bytestr_data)
{
  ptrdiff_t pc = 0;
  unsigned op;
  bool new_bb = true;
  basic_block_t *bb_map = xmalloc (bytestr_length * sizeof (basic_block_t));
  unsigned *bb_start_pc = xmalloc (bytestr_length * sizeof (unsigned));
  unsigned bb_n = 0;

  while (pc < bytestr_length)
    {
      if (new_bb)
	{
	  bb_start_pc[bb_n++] = pc;
	  new_bb = false;
	}

      op = FETCH;
      switch (op)
	{
	  /* 3 byte non branch ops */
	case Bvarref7:
	case Bvarset7:
	case Bvarbind7:
	case Bcall7:
	case Bunbind7:
	case Bstack_ref7:
	case Bstack_set2:
	  pc += 2;
	  break;
	  /* 2 byte non branch ops */
	case Bvarref6:
	case Bvarset6:
	case Bvarbind6:
	case Bcall6:
	case Bunbind6:
	case Bconstant2:
	case BlistN:
	case BconcatN:
	case BinsertN:
	case Bstack_ref6:
	case Bstack_set:
	case BdiscardN:
	  ++pc;
	  break;
	  /* Absolute branches */
	case Bgoto:
	case Bgotoifnil:
	case Bgotoifnonnil:
	case Bgotoifnilelsepop:
	case Bgotoifnonnilelsepop:
	case Bpushcatch:
	case Bpushconditioncase:
	  op = FETCH2;
	  bb_start_pc[bb_n++] = op;
	  new_bb = true;
	  break;
	  /* PC relative branches */
	case BRgoto:
	case BRgotoifnil:
	case BRgotoifnonnil:
	case BRgotoifnilelsepop:
	case BRgotoifnonnilelsepop:
	  op = FETCH - 128;
	  bb_start_pc[bb_n++] = op;
	  new_bb = true;
	  break;
	  /* Other ops changing bb */
	case Bsub1:
	case Badd1:
	case Bnegate:
	case Breturn:
	  new_bb = true;
	  break;
	default:
	  break;
	}
    }

  /* Sort and remove possible duplicates.  */
  qsort (bb_start_pc, bb_n, sizeof(unsigned), ucmp);
  {
    unsigned i, j;
    for (i = j = 0; i < bb_n; i++)
      if (bb_start_pc[i] != bb_start_pc[j])
	bb_start_pc[++j] = bb_start_pc[i];
    bb_n = j + 1;
  }

  basic_block_t curr_bb;
  for (int i = 0, pc = 0; pc < bytestr_length; pc++)
    {
      if (i < bb_n && pc == bb_start_pc[i])
	{
	  ++i;
	  curr_bb.gcc_bb =
	    gcc_jit_function_new_block (comp.func, format_string ("bb_%d", i));
	  curr_bb.top = NULL;
	  curr_bb.terminated = false;
	}
      bb_map[pc] = curr_bb;
    }

  xfree (bb_start_pc);

  return bb_map;
}

static void
init_comp (int opt_level)
{
  comp.ctxt = gcc_jit_context_acquire();

  if (COMP_DEBUG)
    {
      logfile = fopen ("libgccjit.log", "w");
      gcc_jit_context_set_logfile (comp.ctxt,
				   logfile,
				   0, 0);
      gcc_jit_context_set_bool_option (comp.ctxt,
				       GCC_JIT_BOOL_OPTION_KEEP_INTERMEDIATES,
				       1);
    }
  if (COMP_DEBUG > 1)
    {
      gcc_jit_context_set_bool_option (comp.ctxt,
				       GCC_JIT_BOOL_OPTION_DEBUGINFO,
				       1);
      gcc_jit_context_set_bool_option (comp.ctxt,
				       GCC_JIT_BOOL_OPTION_DUMP_INITIAL_GIMPLE,
				       1);

      gcc_jit_context_dump_reproducer_to_file (comp.ctxt, "comp_reproducer.c");

    }

  gcc_jit_context_set_int_option (comp.ctxt,
				  GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL,
				  opt_level);

  /* Do not inline within a compilation unit.  */
  gcc_jit_context_add_command_line_option (comp.ctxt, "-fno-inline");


  comp.void_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_VOID);
  comp.void_ptr_type =
    gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_VOID_PTR);
  comp.bool_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_BOOL);
  comp.char_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_CHAR);
  comp.int_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_INT);
  comp.unsigned_type = gcc_jit_context_get_type (comp.ctxt,
						 GCC_JIT_TYPE_UNSIGNED_INT);
  comp.long_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_LONG);
  comp.unsigned_long_type =
    gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_UNSIGNED_LONG);
  comp.long_long_type =
    gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_LONG_LONG);
  comp.unsigned_long_long_type =
    gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_UNSIGNED_LONG_LONG);
  comp.char_ptr_type = gcc_jit_type_get_pointer (comp.char_type);

#if EMACS_INT_MAX <= LONG_MAX
  /* 32-bit builds without wide ints, 64-bit builds on Posix hosts.  */
  comp.lisp_obj_as_ptr = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    comp.void_ptr_type,
						    "obj");
#else
  /* 64-bit builds on MS-Windows, 32-bit builds with wide ints.	 */
  comp.lisp_obj_as_ptr = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    comp.long_long_type,
						    "obj");
#endif

  comp.emacs_int_type = gcc_jit_context_get_int_type (comp.ctxt,
						      sizeof (EMACS_INT),
						      true);

  comp.lisp_obj_as_num =  gcc_jit_context_new_field (comp.ctxt,
						     NULL,
						     comp.emacs_int_type,
						     "num");

  gcc_jit_field *lisp_obj_fields[] = { comp.lisp_obj_as_ptr,
				       comp.lisp_obj_as_num };
  comp.lisp_obj_type = gcc_jit_context_new_union_type (comp.ctxt,
						       NULL,
						       "comp_Lisp_Object",
						       sizeof (lisp_obj_fields)
						       / sizeof (*lisp_obj_fields),
						       lisp_obj_fields);
  comp.lisp_obj_ptr_type = gcc_jit_type_get_pointer (comp.lisp_obj_type);

  comp.most_positive_fixnum =
    gcc_jit_context_new_rvalue_from_long (comp.ctxt,
					  comp.emacs_int_type,
					  MOST_POSITIVE_FIXNUM);
  comp.most_negative_fixnum =
    gcc_jit_context_new_rvalue_from_long (comp.ctxt,
					  comp.emacs_int_type,
					  MOST_NEGATIVE_FIXNUM);
  comp.one =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.emacs_int_type,
					 1);
  comp.inttypebits =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.emacs_int_type,
					 INTTYPEBITS);

  comp.lisp_int0 =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.emacs_int_type,
					 Lisp_Int0);

  comp.ptrdiff_type = gcc_jit_context_get_int_type (comp.ctxt,
						    sizeof (void *),
						    true);

  comp.uintptr_type = gcc_jit_context_get_int_type (comp.ctxt,
						    sizeof (void *),
						    false);

  comp.func_hash = CALLN (Fmake_hash_table, QCtest, Qequal, QCweakness, Qt);

  /* Define data structures.  */

  define_lisp_cons ();
  define_jmp_buf ();
  define_handler_struct ();
  define_thread_state_struct ();
  define_cast_union ();

  comp.current_thread =
    gcc_jit_context_new_rvalue_from_ptr (comp.ctxt,
					 comp.thread_state_ptr_type,
					 current_thread);
  comp.pure =
    gcc_jit_context_new_rvalue_from_ptr (comp.ctxt,
					 comp.void_ptr_type,
					 pure);

  /* Define inline functions.  */

  define_CAR_CDR();
  define_PSEUDOVECTORP ();
  define_CHECK_TYPE ();
  define_CHECK_IMPURE ();
  define_bool_to_lisp_obj ();
  define_setcar_setcdr();
}

static void
release_comp (void)
{
  if (COMP_DEBUG)
    gcc_jit_context_dump_to_file (comp.ctxt, "gcc-ctxt-dump.c", 1);
  if (comp.ctxt)
    gcc_jit_context_release(comp.ctxt);

  if (logfile)
    fclose (logfile);
}

static comp_f_res_t
compile_f (const char *lisp_f_name, const char *c_f_name,
	   ptrdiff_t bytestr_length, unsigned char *bytestr_data,
	   EMACS_INT stack_depth, Lisp_Object *vectorp,
	   ptrdiff_t vector_size, Lisp_Object args_template)
{
  gcc_jit_rvalue *res;
  comp_f_res_t comp_res = { NULL, 0, 0 };
  ptrdiff_t pc = 0;
  gcc_jit_rvalue *args[MAX_POP];
  unsigned op;
  unsigned pushhandler_n  = 0;

  USE_SAFE_ALLOCA;

  /* Meta-stack we use to flat the bytecode written for push and pop
     Emacs VM.*/
  stack_el_t *stack_base, *stack, *stack_over;
  SAFE_NALLOCA (stack_base, sizeof (stack_el_t), stack_depth);
  stack = stack_base;
  stack_over = stack_base + stack_depth;

  if (FIXNUMP (args_template))
    {
      ptrdiff_t at = XFIXNUM (args_template);
      bool rest = (at & 128) != 0;
      int mandatory = at & 127;
      ptrdiff_t nonrest = at >> 8;

      comp_res.min_args = mandatory;

      eassert (!rest);

      if (!rest && nonrest < SUBR_MAX_ARGS)
	comp_res.max_args = nonrest;
    }
  else if (CONSP (args_template))
    /* FIXME */
    comp_res.min_args = comp_res.max_args = XFIXNUM (Flength (args_template));

  else
    eassert (SYMBOLP (args_template) && args_template == Qnil);


  /* Current function being compiled.  */
  comp.func = emit_func_declare (c_f_name, comp.lisp_obj_type, comp_res.max_args,
				 NULL, GCC_JIT_FUNCTION_EXPORTED, false);

  gcc_jit_lvalue *meta_stack_array =
    gcc_jit_function_new_local (
      comp.func,
      NULL,
      gcc_jit_context_new_array_type (comp.ctxt,
				      NULL,
				      comp.lisp_obj_type,
				      stack_depth),
      "local");

  for (int i = 0; i < stack_depth; ++i)
    stack[i].gcc_lval = gcc_jit_context_new_array_access (
		      comp.ctxt,
		      NULL,
		      gcc_jit_lvalue_as_rvalue (meta_stack_array),
		      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
							   comp.int_type,
							   i));

  DECL_AND_SAFE_ALLOCA_BLOCK(prologue, comp.func);

  basic_block_t *bb_map = compute_blocks (bytestr_length, bytestr_data);

  for (ptrdiff_t i = 0; i < comp_res.max_args; ++i)
    PUSH_PARAM (gcc_jit_function_get_param (comp.func, i));
  gcc_jit_block_end_with_jump (prologue->gcc_bb, NULL, bb_map[0].gcc_bb);

  comp.block = &bb_map[0];
  gcc_jit_rvalue *nil = emit_lisp_obj_from_ptr (Qnil);

  comp.block = NULL;

  while (pc < bytestr_length)
    {
      enum handlertype type;

      /* If we are changing BB and the last was one wasn't terminated
	 terminate it with a fall through.  */
      if (comp.block && comp.block->gcc_bb != bb_map[pc].gcc_bb &&
	  !comp.block->terminated)
	{
	  gcc_jit_block_end_with_jump (comp.block->gcc_bb, NULL, bb_map[pc].gcc_bb);
	  comp.block->terminated = true;
	}
      comp.block = &bb_map[pc];
      if (bb_map[pc].top)
	stack = bb_map[pc].top;
      op = FETCH;

      switch (op)
	{
	CASE (Bstack_ref1);
	  goto stack_ref;
	CASE (Bstack_ref2);
	  goto stack_ref;
	CASE (Bstack_ref3);
	  goto stack_ref;
	CASE (Bstack_ref4);
	  goto stack_ref;
	CASE (Bstack_ref5);
	  stack_ref:
	  PUSH_LVAL (
	    stack_base[(stack - stack_base) - (op - Bstack_ref) - 1].gcc_lval);
	  break;

	CASE (Bstack_ref6);
	  PUSH_LVAL (stack_base[(stack - stack_base) - FETCH - 1].gcc_lval);
	  break;

	CASE (Bstack_ref7);
	  PUSH_LVAL (stack_base[(stack - stack_base) - FETCH2 - 1].gcc_lval);
	  break;

	CASE (Bvarref7);
	  op = FETCH2;
	  goto varref;

	CASE (Bvarref);
	  goto varref_count;
	CASE (Bvarref1);
	  goto varref_count;
	CASE (Bvarref2);
	  goto varref_count;
	CASE (Bvarref3);
	  goto varref_count;
	CASE (Bvarref4);
	  goto varref_count;
	CASE (Bvarref5);
	  varref_count:
	  op -= Bvarref;
	  goto varref;

	CASE (Bvarref6);
	  op = FETCH;
	varref:
	  {
	    args[0] = emit_lisp_obj_from_ptr (vectorp[op]);
	    res = emit_call ("Fsymbol_value", comp.lisp_obj_type, 1, args);
	    PUSH_RVAL (res);
	    break;
	  }

	CASE (Bvarset);
	  goto varset_count;
	CASE (Bvarset1);
	  goto varset_count;
	CASE (Bvarset2);
	  goto varset_count;
	CASE (Bvarset3);
	  goto varset_count;
	CASE (Bvarset4);
	  goto varset_count;
	CASE (Bvarset5);
	  varset_count:
	  op -= Bvarset;
	  goto varset;

	CASE (Bvarset7);
	  op = FETCH2;
	  goto varset;

	CASE (Bvarset6);
	  op = FETCH;
	varset:
	  {
	    POP1;
	    args[1] = args[0];
	    args[0] = emit_lisp_obj_from_ptr (vectorp[op]);
	    args[2] = nil;
	    args[3] = gcc_jit_context_new_rvalue_from_int (comp.ctxt,
							   comp.int_type,
							   SET_INTERNAL_SET);
	    res = emit_call ("set_internal", comp.lisp_obj_type, 4, args);
	    PUSH_RVAL (res);
	  }
	  break;

	CASE (Bvarbind6);
	  op = FETCH;
	  goto varbind;

	CASE (Bvarbind7);
	  op = FETCH2;
	  goto varbind;

	CASE (Bvarbind);
	  goto varbind_count;
	CASE (Bvarbind1);
	  goto varbind_count;
	CASE (Bvarbind2);
	  goto varbind_count;
	CASE (Bvarbind3);
	  goto varbind_count;
	CASE (Bvarbind4);
	  goto varbind_count;
	CASE (Bvarbind5);
	  varbind_count:
	  op -= Bvarbind;
	varbind:
	  {
	    args[0] = emit_lisp_obj_from_ptr (vectorp[op]);
	    pop (1, &stack, &args[1]);
	    res = emit_call ("specbind", comp.lisp_obj_type, 2, args);
	    PUSH_RVAL (res);
	    break;
	  }

	CASE (Bcall6);
	  op = FETCH;
	  goto docall;

	CASE (Bcall7);
	  op = FETCH2;
	  goto docall;

	CASE (Bcall);
	  goto docall_count;
	CASE (Bcall1);
	  goto docall_count;
	CASE (Bcall2);
	  goto docall_count;
	CASE (Bcall3);
	  goto docall_count;
	CASE (Bcall4);
	  goto docall_count;
	CASE (Bcall5);
	docall_count:
	  op -= Bcall;
	docall:
	  {
	    res = NULL;
	    ptrdiff_t nargs = op + 1;
	    pop (nargs, &stack, args);
	    if (stack->const_set &&
		stack->type == Lisp_Symbol)
	      {
		ptrdiff_t native_nargs = op;
		char *sym_name = (char *) SDATA (SYMBOL_NAME (stack->constant));
		if (!strcmp (sym_name,
			     lisp_f_name))
		  {
		    /* Optimize self calls.  */
		    res = gcc_jit_context_new_call (comp.ctxt,
						    NULL,
						    comp.func,
						    native_nargs,
						    args + 1);
		  } else if (SUBRP ((XSYMBOL (stack->constant)->u.s.function)))
		  {
		    /* Optimize primitive native calls.  */
		    emit_comment (format_string ("Calling primitive %s",
						 sym_name));
		    struct Lisp_Subr *subr =
		      XSUBR ((XSYMBOL (stack->constant)->u.s.function));
		    if (subr->max_args == MANY)
		      {
			/* FIXME: do we want to optimize this case too?  */
			goto dofuncall;
		      } else
		      {
			gcc_jit_type *types[native_nargs];

			for (int i = 0; i < native_nargs; i++)
			  types[i] = comp.lisp_obj_type;

			gcc_jit_type *fn_ptr_type =
			  gcc_jit_context_new_function_ptr_type (
			    comp.ctxt,
			    NULL,
			    comp.lisp_obj_type,
			    native_nargs,
			    types,
			    0);
			res =
			  gcc_jit_context_new_call_through_ptr (
			    comp.ctxt,
			    NULL,
			    gcc_jit_context_new_rvalue_from_ptr (
			      comp.ctxt,
			      fn_ptr_type,
			      subr->function.a0),
			    native_nargs,
			    args + 1);
		      }
		  }
	      }
	  dofuncall:
	    /* Fall back to regular funcall dispatch mechanism. */
	    if (!res)
	      res = emit_call_n_ref ("Ffuncall", nargs, stack->gcc_lval);

	    PUSH_RVAL (res);
	    break;
	  }

	CASE (Bunbind6);
	  op = FETCH;
	  goto dounbind;

	CASE (Bunbind7);
	  op = FETCH2;
	  goto dounbind;

	CASE (Bunbind);
	  goto dounbind_count;
	CASE (Bunbind1);
	  goto dounbind_count;
	CASE (Bunbind2);
	  goto dounbind_count;
	CASE (Bunbind3);
	  goto dounbind_count;
	CASE (Bunbind4);
	  goto dounbind_count;
	CASE (Bunbind5);
	dounbind_count:
	  op -= Bunbind;
	dounbind:
	  {
	    args[0] = gcc_jit_context_new_rvalue_from_int(comp.ctxt,
							  comp.ptrdiff_type,
							  op);

	    emit_call ("helper_unbind_n", comp.lisp_obj_type, 1, args);
	  }
	  break;

	CASE (Bpophandler);
	  {
	  /* current_thread->m_handlerlist =
	       current_thread->m_handlerlist->next;  */
	    gcc_jit_lvalue *m_handlerlist =
	      gcc_jit_rvalue_dereference_field (comp.current_thread,
						NULL,
						comp.m_handlerlist);

	    gcc_jit_block_add_assignment(
	      comp.block->gcc_bb,
	      NULL,
	      m_handlerlist,
	      gcc_jit_lvalue_as_rvalue (
		gcc_jit_rvalue_dereference_field (
			gcc_jit_lvalue_as_rvalue (m_handlerlist),
			NULL,
			comp.handler_next_field)));
	    break;
	  }

	CASE (Bpushconditioncase); /* New in 24.4.  */
	  type = CONDITION_CASE;
	  goto pushhandler;

	CASE (Bpushcatch);	/* New in 24.4.	 */
	  type = CATCHER;
	pushhandler:
	  {
	    /* struct handler *c = push_handler (POP, type); */
	    int handler_pc = FETCH2;
	    gcc_jit_lvalue *c =
	      gcc_jit_function_new_local (comp.func,
					  NULL,
					  comp.handler_ptr_type,
					  format_string ("c_%u",
							 pushhandler_n));
	    POP1;
	    args[1] = gcc_jit_context_new_rvalue_from_int (comp.ctxt,
							   comp.int_type,
							   type);
	    gcc_jit_block_add_assignment (
	      comp.block->gcc_bb,
	      NULL,
	      c,
	      emit_call ("push_handler", comp.handler_ptr_type, 2, args));

	    args[0] =
	      gcc_jit_lvalue_get_address (
		gcc_jit_rvalue_dereference_field (
		  gcc_jit_lvalue_as_rvalue (c),
		  NULL,
		  comp.handler_jmp_field),
		NULL);
#ifdef HAVE__SETJMP
	    res = emit_call ("_setjmp", comp.int_type, 1, args);
#else
	    res = emit_call ("setjmp", comp.int_type, 1, args);
#endif
	    basic_block_t *push_h_val_block;
	    SAFE_ALLOCA_BLOCK (push_h_val_block,
			       comp.func,
			       format_string ("push_h_val_%u",
					      pushhandler_n));

	    emit_cond_jump (res, push_h_val_block, &bb_map[pc]);

	    stack_el_t *stack_to_restore = stack;
	    /* This emit the handler part.  */

	    basic_block_t *bb_orig = comp.block;
	    comp.block = push_h_val_block;
	    /* current_thread->m_handlerlist = c->next; */
	    gcc_jit_lvalue *m_handlerlist =
	      gcc_jit_rvalue_dereference_field (comp.current_thread,
						NULL,
						comp.m_handlerlist);
	    gcc_jit_block_add_assignment (comp.block->gcc_bb,
					  NULL,
					  m_handlerlist,
					  gcc_jit_lvalue_as_rvalue(
					    gcc_jit_rvalue_dereference_field (
					      gcc_jit_lvalue_as_rvalue (c),
					      NULL,
					      comp.handler_next_field)));
	    /* PUSH (c->val); */
	    PUSH_LVAL (gcc_jit_rvalue_dereference_field (
			 gcc_jit_lvalue_as_rvalue (c),
			 NULL,
			 comp.handler_val_field));
	    bb_map[handler_pc].top = stack;
	    comp.block = bb_orig;

	    gcc_jit_block_end_with_jump (push_h_val_block->gcc_bb, NULL,
					 bb_map[handler_pc].gcc_bb);

	    stack = stack_to_restore;
	    ++pushhandler_n;
	  }
	  break;

	CASE_CALL_N (nth, 2);
	CASE_CALL_N (symbolp, 1);

	CASE (Bconsp);
	  POP1;
	  res = emit_cast (comp.bool_type,
			   emit_CONSP (args[0]));
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.bool_to_lisp_obj,
					  1, &res);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (stringp, 1);
	CASE_CALL_N (listp, 1);
	CASE_CALL_N (eq, 2);
	CASE_CALL_N (memq, 1);
	CASE_CALL_N (not, 1);

	case Bcar:
	  POP1;
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.car,
					  1, args);
	  PUSH_RVAL (res);
	  break;

	  case Bcdr:
	  POP1;
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.cdr,
					  1, args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (cons, 2);

	CASE (BlistN);
	  op = FETCH;
	  goto make_list;

	CASE (Blist1);
	  goto make_list_count;
	CASE (Blist2);
	  goto make_list_count;
	CASE (Blist3);
	  goto make_list_count;
	CASE (Blist4);
	make_list_count:
	  op = op - Blist1;
	make_list:
	  {
	    POP1;
	    args[1] = nil;
	    res = emit_call ("Fcons", comp.lisp_obj_type, 2, args);
	    PUSH_RVAL (res);
	    for (int i = 0; i < op; ++i)
	      {
		POP2;
		res = emit_call ("Fcons", comp.lisp_obj_type, 2, args);
		PUSH_RVAL (res);
	      }
	    break;
	  }

	CASE_CALL_N (length, 1);
	CASE_CALL_N (aref, 2);
	CASE_CALL_N (aset, 3);
	CASE_CALL_N (symbol_value, 1);
	CASE_CALL_N (symbol_function, 1);
	CASE_CALL_N (set, 2);
	CASE_CALL_N (fset, 2);
	CASE_CALL_N (get, 2);
	CASE_CALL_N (substring, 3);

	CASE (Bconcat2);
	  EMIT_CALL_N_REF ("Fconcat", 2);
	  break;
	CASE (Bconcat3);
	  EMIT_CALL_N_REF ("Fconcat", 3);
	  break;
	CASE (Bconcat4);
	  EMIT_CALL_N_REF ("Fconcat", 4);
	  break;
	CASE (BconcatN);
	  op = FETCH;
	  EMIT_CALL_N_REF ("Fconcat", op);
	  break;

	CASE (Bsub1);
	  {
	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_NEGATIVE_FIXNUM
		 ? make_fixnum (XFIXNUM (TOP) - 1)
		 : Fsub1 (TOP)) */

	    DECL_AND_SAFE_ALLOCA_BLOCK (sub1_inline_block, comp.func);
	    DECL_AND_SAFE_ALLOCA_BLOCK (sub1_fcall_block, comp.func);

	    gcc_jit_rvalue *tos_as_num =
	      emit_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval));

	    emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		emit_cast (comp.bool_type,
			   emit_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_negative_fixnum)),
	      sub1_inline_block,
	      sub1_fcall_block);

	    gcc_jit_rvalue *sub1_inline_res =
	      gcc_jit_context_new_binary_op (comp.ctxt,
					     NULL,
					     GCC_JIT_BINARY_OP_MINUS,
					     comp.emacs_int_type,
					     tos_as_num,
					     comp.one);

	    basic_block_t *bb_orig = comp.block;

	    comp.block = sub1_inline_block;
	    emit_assign_to_stack_slot (sub1_inline_block,
				       &TOS,
				       emit_make_fixnum (sub1_inline_res));
	    comp.block = sub1_fcall_block;
	    POP1;
	    res = emit_call ("Fsub1", comp.lisp_obj_type, 1, args);
	    PUSH_RVAL (res);

	    gcc_jit_block_end_with_jump (sub1_inline_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (sub1_fcall_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    comp.block = bb_orig;
	    SAFE_FREE ();
	  }

	  break;
	CASE (Badd1);
	  {
	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_POSITIVE_FIXNUM
		 ? make_fixnum (XFIXNUM (TOP) + 1)
		 : Fadd (TOP)) */

	    DECL_AND_SAFE_ALLOCA_BLOCK (add1_inline_block, comp.func);
	    DECL_AND_SAFE_ALLOCA_BLOCK (add1_fcall_block, comp.func);

	    gcc_jit_rvalue *tos_as_num =
	      emit_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval));

	    emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		emit_cast (comp.bool_type,
			   emit_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_positive_fixnum)),
	      add1_inline_block,
	      add1_fcall_block);

	    gcc_jit_rvalue *add1_inline_res =
	      gcc_jit_context_new_binary_op (comp.ctxt,
					     NULL,
					     GCC_JIT_BINARY_OP_PLUS,
					     comp.emacs_int_type,
					     tos_as_num,
					     comp.one);

	    basic_block_t *bb_orig = comp.block;
	    comp.block = add1_inline_block;
	    emit_assign_to_stack_slot(add1_inline_block,
				      &TOS,
				      emit_make_fixnum (add1_inline_res));
	    comp.block = add1_fcall_block;
	    POP1;
	    res = emit_call ("Fadd1", comp.lisp_obj_type, 1, args);
	    PUSH_RVAL (res);

	    gcc_jit_block_end_with_jump (add1_inline_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (add1_fcall_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    comp.block = bb_orig;
	    SAFE_FREE ();
	  }
	  break;

	CASE (Beqlsign);
	  EMIT_ARITHCOMPARE (ARITH_EQUAL);
	  break;

	CASE (Bgtr);
	  EMIT_ARITHCOMPARE (ARITH_GRTR);
	  break;

	CASE (Blss);
	  EMIT_ARITHCOMPARE (ARITH_LESS);
	  break;

	CASE (Bleq);
	  EMIT_ARITHCOMPARE (ARITH_LESS_OR_EQUAL);
	  break;

	CASE (Bgeq);
	  EMIT_ARITHCOMPARE (ARITH_GRTR_OR_EQUAL);
	  break;

	CASE (Bdiff);
	  EMIT_CALL_N_REF ("Fminus", 2);
	  break;

	CASE (Bnegate);
	  {
	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_NEGATIVE_FIXNUM
		 ? make_fixnum (- XFIXNUM (TOP))
		 : Fminus (1, &TOP)) */

	    DECL_AND_SAFE_ALLOCA_BLOCK (negate_inline_block, comp.func);
	    DECL_AND_SAFE_ALLOCA_BLOCK (negate_fcall_block, comp.func);

	    gcc_jit_rvalue *tos_as_num =
	      emit_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval));

	    emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		emit_cast (comp.bool_type,
			   emit_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS.gcc_lval))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_negative_fixnum)),
	      negate_inline_block,
	      negate_fcall_block);

	    gcc_jit_rvalue *negate_inline_res =
	      gcc_jit_context_new_unary_op (comp.ctxt,
					    NULL,
					    GCC_JIT_UNARY_OP_MINUS,
					    comp.emacs_int_type,
					    tos_as_num);

	    basic_block_t *bb_orig = comp.block;

	    comp.block = negate_inline_block;
	    emit_assign_to_stack_slot (negate_inline_block,
				       &TOS,
				       emit_make_fixnum (negate_inline_res));
	    comp.block = negate_fcall_block;
	    EMIT_CALL_N_REF ("Fminus", 1);

	    gcc_jit_block_end_with_jump (negate_inline_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (negate_fcall_block->gcc_bb,
					 NULL,
					 bb_map[pc].gcc_bb);
	    comp.block = bb_orig;
	    SAFE_FREE ();
	  }
	  break;
	CASE (Bplus);
	  EMIT_CALL_N_REF ("Fplus", 2);
	  break;
	CASE (Bmax);
	  EMIT_CALL_N_REF ("Fmax", 2);
	  break;
	CASE (Bmin);
	  EMIT_CALL_N_REF ("Fmin", 2);
	  break;
	CASE (Bmult);
	  EMIT_CALL_N_REF ("Ftimes", 2);
	  break;
	CASE (Bpoint);
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 PT);
	  res = emit_call ("make_fixed_natnum",
			   comp.lisp_obj_type,
			   1,
			   args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (goto_char, 1);

	CASE (Binsert);
	  EMIT_CALL_N_REF ("Finsert", 1);
	  break;

	CASE (Bpoint_max);
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 ZV);
	  res = emit_call ("make_fixed_natnum",
			   comp.lisp_obj_type,
			   1,
			   args);
	  PUSH_RVAL (res);
	  break;

	CASE (Bpoint_min);
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 BEGV);
	  res = emit_call ("make_fixed_natnum",
			   comp.lisp_obj_type,
			   1,
			   args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (char_after, 1);
	CASE_CALL_N (following_char, 0);

	CASE (Bpreceding_char);
	  res = emit_call ("Fprevious_char", comp.lisp_obj_type, 0, args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (current_column, 0);

	CASE (Bindent_to);
	  POP1;
	  args[1] = nil;
	  res = emit_call ("Findent_to", comp.lisp_obj_type, 2, args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (eolp, 0);
	CASE_CALL_N (eobp, 0);
	CASE_CALL_N (bolp, 0);
	CASE_CALL_N (bobp, 0);
	CASE_CALL_N (current_buffer, 0);
	CASE_CALL_N (set_buffer, 1);

	CASE (Bsave_current_buffer); /* Obsolete since ??.  */
	  goto save_current;
	CASE (Bsave_current_buffer_1);
	  save_current:
	  emit_call ("record_unwind_current_buffer",
		     comp.void_type, 0, NULL);
	  break;

	CASE (Binteractive_p);	/* Obsolete since 24.1.	 */
	  PUSH_RVAL (emit_lisp_obj_from_ptr (intern ("interactive-p")));
	  res = emit_call ("call0", comp.lisp_obj_type, 1, args);
	  PUSH_RVAL (res);
	  break;

	CASE_CALL_N (forward_char, 1);
	CASE_CALL_N (forward_word, 1);
	CASE_CALL_N (skip_chars_forward, 2);
	CASE_CALL_N (skip_chars_backward, 2);
	CASE_CALL_N (forward_line, 1);
	CASE_CALL_N (char_syntax, 1);
	CASE_CALL_N (buffer_substring, 2);
	CASE_CALL_N (delete_region, 2);
	CASE_CALL_N (narrow_to_region, 2);
	CASE_CALL_N (widen, 0);
	CASE_CALL_N (end_of_line, 1);

	CASE (Bconstant2);
	  goto do_constant;
	  break;

	CASE (Bgoto);
	  op = FETCH2;
	  gcc_jit_block_end_with_jump (comp.block->gcc_bb,
				       NULL,
				       bb_map[op].gcc_bb);
	  comp.block->terminated = true;
	  bb_map[op].top = stack;
	  break;

	CASE (Bgotoifnil);
	  op = FETCH2;
	  POP1;
	  emit_comparison_jump (GCC_JIT_COMPARISON_EQ, args[0], nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  break;

	CASE (Bgotoifnonnil);
	  op = FETCH2;
	  POP1;
	  emit_comparison_jump (GCC_JIT_COMPARISON_NE, args[0], nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  break;

	CASE (Bgotoifnilelsepop);
	  op = FETCH2;
	  emit_comparison_jump (GCC_JIT_COMPARISON_EQ,
				gcc_jit_lvalue_as_rvalue (TOS.gcc_lval),
				nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  DISCARD (1);
	  break;

	CASE (Bgotoifnonnilelsepop);
	  op = FETCH2;
	  emit_comparison_jump (GCC_JIT_COMPARISON_NE,
				gcc_jit_lvalue_as_rvalue (TOS.gcc_lval),
				nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  DISCARD (1);
	  break;

	CASE (Breturn);
	  POP1;
	  gcc_jit_block_end_with_return(comp.block->gcc_bb,
					NULL,
					args[0]);
	  comp.block->terminated = true;
	  break;

	CASE (Bdiscard);
	  DISCARD (1);
	  break;

	CASE (Bdup);
	  PUSH_LVAL (TOS.gcc_lval);
	  break;

	CASE (Bsave_excursion);
	  res = emit_call ("record_unwind_protect_excursion",
			   comp.void_type, 0, args);
	  break;

	CASE (Bsave_window_excursion); /* Obsolete since 24.1.  */
	  EMIT_CALL_N ("helper_save_window_excursion", 1);
	  break;

	CASE (Bsave_restriction);
	  args[0] = emit_lisp_obj_from_ptr (save_restriction_restore);
	  args[1] = emit_call ("save_restriction_save",
			       comp.lisp_obj_type,
			       0,
			       NULL);
	  emit_call ("record_unwind_protect", comp.void_ptr_type, 2, args);
	  break;

	CASE (Bcatch);		/* Obsolete since 24.4.	 */
	  POP2;
	  args[2] = args[1];
	  args[1] = emit_lisp_obj_from_ptr (eval_sub);
	  emit_call ("internal_catch", comp.void_ptr_type, 3, args);
	  break;

	CASE (Bunwind_protect);	/* FIXME: avoid closure for lexbind.  */
	  POP1;
	  emit_call ("helper_unwind_protect", comp.void_type, 1, args);
	  break;

	CASE (Bcondition_case);		/* Obsolete since 24.4.	 */
	  POP3;
	  emit_call ("internal_lisp_condition_case",
		     comp.lisp_obj_type, 3, args);
	  break;

	CASE (Btemp_output_buffer_setup); /* Obsolete since 24.1.	 */
	  EMIT_CALL_N ("helper_temp_output_buffer_setup", 1);
	  break;

	CASE (Btemp_output_buffer_show); /* Obsolete since 24.1.	*/
	  POP2;
	  emit_call ("temp_output_buffer_show", comp.void_type, 1,
		     &args[1]);
	  PUSH_RVAL (args[0]);
	  emit_call ("helper_unbind_n", comp.lisp_obj_type, 1, args);

	  break;
	CASE (Bunbind_all);	/* Obsolete.  Never used.  */
	  /* To unbind back to the beginning of this frame.  Not used yet,
	     but will be needed for tail-recursion elimination.	 */
	  error ("Bunbind_all not supported");
	  break;

	CASE_CALL_N (set_marker, 3);
	CASE_CALL_N (match_beginning, 1);
	CASE_CALL_N (match_end, 1);
	CASE_CALL_N (upcase, 1);
	CASE_CALL_N (downcase, 1);

	CASE (Bstringeqlsign);
	  EMIT_CALL_N ("Fstring_equal", 2);
	  break;

	CASE (Bstringlss);
	  EMIT_CALL_N ("Fstring_lessp", 2);
	  break;

	CASE_CALL_N (equal, 2);
	CASE_CALL_N (nthcdr, 2);
	CASE_CALL_N (elt, 2);
	CASE_CALL_N (member, 2);
	CASE_CALL_N (assq, 2);

	case Bsetcar:
	  POP2;
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.setcar,
					  2, args);
	  PUSH_RVAL (res);
	  break;

	case Bsetcdr:
	  POP2;
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.setcdr,
					  2, args);
	  PUSH_RVAL (res);
	  break;

	CASE (Bcar_safe);
	  EMIT_CALL_N ("CAR_SAFE", 1);
	  break;

	CASE (Bcdr_safe);
	  EMIT_CALL_N ("CDR_SAFE", 1);
	  break;

	CASE (Bnconc);
	  EMIT_CALL_N_REF ("Fnconc", 2);
	  break;

	CASE (Bquo);
	  EMIT_CALL_N_REF ("Fquo", 2);
	  break;

	CASE_CALL_N (rem, 2);

	CASE (Bnumberp);
	  POP1;
	  res = emit_NUMBERP (args[0]);
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.bool_to_lisp_obj,
					  1, &res);
	  PUSH_RVAL (res);
	  break;

	CASE (Bintegerp);
	  POP1;
	  res = emit_INTEGERP(args[0]);
	  res = gcc_jit_context_new_call (comp.ctxt,
					  NULL,
					  comp.bool_to_lisp_obj,
					  1, &res);
	  PUSH_RVAL (res);
	  break;

	CASE (BRgoto);
	  op = FETCH - 128;
	  op += pc;
	  gcc_jit_block_end_with_jump (comp.block->gcc_bb,
				       NULL,
				       bb_map[op].gcc_bb);
	  comp.block->terminated = true;
	  bb_map[op].top = stack;
	  break;

	CASE (BRgotoifnil);
	  op = FETCH - 128;
	  op += pc;
	  POP1;
	  emit_comparison_jump (GCC_JIT_COMPARISON_EQ, args[0], nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  break;

	CASE (BRgotoifnonnil);
	  op = FETCH - 128;
	  op += pc;
	  POP1;
	  emit_comparison_jump (GCC_JIT_COMPARISON_NE, args[0], nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  break;

	CASE (BRgotoifnilelsepop);
	  op = FETCH - 128;
	  op += pc;
	  emit_comparison_jump (GCC_JIT_COMPARISON_EQ,
				gcc_jit_lvalue_as_rvalue (TOS.gcc_lval),
				nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  DISCARD (1);
	  break;

	CASE (BRgotoifnonnilelsepop);
	  op = FETCH - 128;
	  op += pc;
	  emit_comparison_jump (GCC_JIT_COMPARISON_NE,
				gcc_jit_lvalue_as_rvalue (TOS.gcc_lval),
				nil,
				&bb_map[op], &bb_map[pc]);
	  bb_map[op].top = stack;
	  DISCARD (1);
	  break;

	CASE (BinsertN);
	  op = FETCH;
	  EMIT_CALL_N_REF ("Finsert", op);
	  break;

	CASE (Bstack_set);
	  /* stack-set-0 = discard; stack-set-1 = discard-1-preserve-tos.  */
	  op = FETCH;
	  POP1;
	  if (op > 0)
	    emit_assign_to_stack_slot (comp.block, stack - op, args[0]);
	  break;

	CASE (Bstack_set2);
	  op = FETCH2;
	  POP1;
	  emit_assign_to_stack_slot (comp.block, stack - op, args[0]);
	  break;

	CASE (BdiscardN);
	  op = FETCH;
	  if (op & 0x80)
	    {
	      op &= 0x7F;
	      POP1;
	      emit_assign_to_stack_slot (comp.block, stack - op - 1, args[0]);
	    }

	  DISCARD (op);
	  break;
	CASE (Bswitch);
	  error ("Bswitch not supported");
	  /* The cases of Bswitch that we handle (which in theory is
	     all of them) are done in Bconstant, below.	 This is done
	     due to a design issue with Bswitch -- it should have
	     taken a constant pool index inline, but instead looks for
	     a constant on the stack.  */
	  goto fail;
	  break;

	default:
	CASE (Bconstant);
	  {
	    if (op < Bconstant || op > Bconstant + vector_size)
	      goto fail;

	    op -= Bconstant;
	  do_constant:

	    /* See the Bswitch case for commentary.  */
	    if (pc >= bytestr_length || bytestr_data[pc] != Bswitch)
	      {
		gcc_jit_rvalue *c =
		  emit_lisp_obj_from_ptr (vectorp[op]);
		PUSH_RVAL (c);
		TOS.type = XTYPE (vectorp[op]);
		if (TOS.type == Lisp_Symbol)
		  {
		    /* Store the symbol value for later use is used while
		       optimizing native and self calls.  */
		    TOS.constant = vectorp[op];
		    TOS.const_set = true;
		  }
		break;
	      }

	    /* We're compiling Bswitch instead.	 */
	    ++pc;
	    break;
	  }
	}
    }

  comp_res.gcc_res = gcc_jit_context_compile(comp.ctxt);

  goto exit;

 fail:
  error ("Something went wrong");

 exit:
  xfree (bb_map);
  SAFE_FREE ();
  return comp_res;
}

void
emacs_native_compile (const char *lisp_f_name, const char *c_f_name,
		      Lisp_Object func, int opt_level, bool dump_asm)
{
  init_comp (opt_level);
  Lisp_Object bytestr = AREF (func, COMPILED_BYTECODE);
  CHECK_STRING (bytestr);

  if (STRING_MULTIBYTE (bytestr))
    /* BYTESTR must have been produced by Emacs 20.2 or the earlier
       because they produced a raw 8-bit string for byte-code and now
       such a byte-code string is loaded as multibyte while raw 8-bit
       characters converted to multibyte form.	Thus, now we must
       convert them back to the originally intended unibyte form.  */
    bytestr = Fstring_as_unibyte (bytestr);

  ptrdiff_t bytestr_length = SBYTES (bytestr);

  Lisp_Object vector = AREF (func, COMPILED_CONSTANTS);
  CHECK_VECTOR (vector);
  Lisp_Object *vectorp = XVECTOR (vector)->contents;

  Lisp_Object maxdepth = AREF (func, COMPILED_STACK_DEPTH);
  CHECK_FIXNAT (maxdepth);

  /* Gcc doesn't like being interrupted.  */
  sigset_t oldset;
  block_atimers (&oldset);

  comp_f_res_t comp_res = compile_f (lisp_f_name, c_f_name, bytestr_length,
				     SDATA (bytestr), XFIXNAT (maxdepth) + 1,
				     vectorp, ASIZE (vector),
				     AREF (func, COMPILED_ARGLIST));

  union Aligned_Lisp_Subr *x = xmalloc (sizeof (union Aligned_Lisp_Subr));

  x->s.header.size = PVEC_SUBR << PSEUDOVECTOR_AREA_BITS;
  x->s.function.a0 = gcc_jit_result_get_code(comp_res.gcc_res, c_f_name);
  eassert (x->s.function.a0);
  x->s.min_args = comp_res.min_args;
  x->s.max_args = comp_res.max_args;
  x->s.symbol_name = lisp_f_name;
  defsubr(x);

  if (dump_asm)
    {
      gcc_jit_context_compile_to_file(comp.ctxt,
				      GCC_JIT_OUTPUT_KIND_ASSEMBLER,
				      DISASS_FILE_NAME);
    }
  unblock_atimers (&oldset);
  release_comp ();
}

DEFUN ("native-compile", Fnative_compile, Snative_compile,
       1, 3, 0,
       doc: /* Compile as native code function FUNC and load it.  */) /* FIXME doc */
     (Lisp_Object func, Lisp_Object speed, Lisp_Object disassemble)
{
  static char c_f_name[MAX_FUN_NAME];
  char *lisp_f_name;

  if (!SYMBOLP (func))
    error ("Not a symbol.");

  lisp_f_name = (char *) SDATA (SYMBOL_NAME (func));

  int res = snprintf (c_f_name, MAX_FUN_NAME, "Fnative_comp_%s", lisp_f_name);

  if (res >= MAX_FUN_NAME)
    error ("Function name too long");

  /* FIXME how many other characters are not allowed in C?
     This will introduce name clashs too. */
  char *c = c_f_name;
  while (*c)
    {
      if (*c == '-' ||
	  *c == '+')
	*c = '_';
      ++c;
    }

  func = indirect_function (func);
  if (!COMPILEDP (func))
    error ("Not a byte-compiled function");

  if (speed != Qnil &&
      (!FIXNUMP (speed) ||
       !(XFIXNUM (speed) >= 0 &&
	 XFIXNUM (speed) <= 3)))
    error ("opt-level must be number between 0 and 3");

  int opt_level;
  if (speed == Qnil)
    opt_level = DEFAULT_SPEED;
  else
    opt_level = XFIXNUM (speed);

  emacs_native_compile (lisp_f_name, c_f_name, func, opt_level,
			!NILP (disassemble));

  if (!NILP (disassemble))
    {
      FILE *fd;
      Lisp_Object str;

      if ((fd = fopen (DISASS_FILE_NAME, "r")))
	{
	  fseek (fd , 0L, SEEK_END);
	  long int size = ftell (fd);
	  fseek (fd , 0L, SEEK_SET);
	  char *buffer = xmalloc (size + 1);
	  ptrdiff_t nread = fread (buffer, 1, size, fd);
	  if (nread > 0)
	    {
	      size = nread;
	      buffer[size] = '\0';
	      str = make_string (buffer, size);
	      fclose (fd);
	    }
	  else
	    str = empty_unibyte_string;
	  xfree (buffer);
	  return str;
	}
      else
	{
	  error ("disassemble file could not be found");
	}
    }

  return Qnil;
}

void
syms_of_comp (void)
{
  defsubr (&Snative_compile);
  comp.func_hash = Qnil;
  staticpro (&comp.func_hash);
}

/******************************************************************************/
/* Helper functions called from the runtime.				      */
/* These can't be statics till shared mechanism is used to solve relocations. */
/******************************************************************************/

Lisp_Object helper_save_window_excursion (Lisp_Object v1);

void helper_unwind_protect (Lisp_Object handler);

Lisp_Object helper_temp_output_buffer_setup (Lisp_Object x);

Lisp_Object helper_unbind_n (int val);

bool helper_PSEUDOVECTOR_TYPEP_XUNTAG (const union vectorlike_header *a,
				       enum pvec_type code);
Lisp_Object
helper_save_window_excursion (Lisp_Object v1)
{
  ptrdiff_t count1 = SPECPDL_INDEX ();
  record_unwind_protect (restore_window_configuration,
			 Fcurrent_window_configuration (Qnil));
  v1 = Fprogn (v1);
  unbind_to (count1, v1);
  return v1;
}

void helper_unwind_protect (Lisp_Object handler)
{
  /* Support for a function here is new in 24.4.  */
  record_unwind_protect (FUNCTIONP (handler) ? bcall0 : prog_ignore,
			 handler);
}

Lisp_Object
helper_temp_output_buffer_setup (Lisp_Object x)
{
  CHECK_STRING (x);
  temp_output_buffer_setup (SSDATA (x));
  return Vstandard_output;
}

Lisp_Object
helper_unbind_n (int val)
{
  return unbind_to (SPECPDL_INDEX () - val, Qnil);
}

bool
helper_PSEUDOVECTOR_TYPEP_XUNTAG (const union vectorlike_header *a,
				  enum pvec_type code)
{
  return PSEUDOVECTOR_TYPEP (XUNTAG (a, Lisp_Vectorlike,
				     union vectorlike_header),
			     code);
}

#endif /* HAVE_LIBGCCJIT */
