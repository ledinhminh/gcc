/* Perform the semantic phase of lambda parsing, i.e., the process of
   building tree structure, checking semantic consistency, and
   building RTL.  These routines are used both during actual parsing
   and during the instantiation of template functions.

   Copyright (C) 1998-2013 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "cgraph.h"
#include "tree-iterator.h"
#include "cp-tree.h"
#include "toplev.h"
#include "vec.h"

/* Constructor for a lambda expression.  */

tree
build_lambda_expr (void)
{
  tree lambda = make_node (LAMBDA_EXPR);
  LAMBDA_EXPR_DEFAULT_CAPTURE_MODE (lambda) = CPLD_NONE;
  LAMBDA_EXPR_CAPTURE_LIST         (lambda) = NULL_TREE;
  LAMBDA_EXPR_THIS_CAPTURE         (lambda) = NULL_TREE;
  LAMBDA_EXPR_PENDING_PROXIES      (lambda) = NULL;
  LAMBDA_EXPR_RETURN_TYPE          (lambda) = NULL_TREE;
  LAMBDA_EXPR_MUTABLE_P            (lambda) = false;
  return lambda;
}

/* Create the closure object for a LAMBDA_EXPR.  */

tree
build_lambda_object (tree lambda_expr)
{
  /* Build aggregate constructor call.
     - cp_parser_braced_list
     - cp_parser_functional_cast  */
  vec<constructor_elt, va_gc> *elts = NULL;
  tree node, expr, type;
  location_t saved_loc;

  if (processing_template_decl)
    return lambda_expr;

  /* Make sure any error messages refer to the lambda-introducer.  */
  saved_loc = input_location;
  input_location = LAMBDA_EXPR_LOCATION (lambda_expr);

  for (node = LAMBDA_EXPR_CAPTURE_LIST (lambda_expr);
       node;
       node = TREE_CHAIN (node))
    {
      tree field = TREE_PURPOSE (node);
      tree val = TREE_VALUE (node);

      if (field == error_mark_node)
	{
	  expr = error_mark_node;
	  goto out;
	}

      if (DECL_P (val))
	mark_used (val);

      /* Mere mortals can't copy arrays with aggregate initialization, so
	 do some magic to make it work here.  */
      if (TREE_CODE (TREE_TYPE (field)) == ARRAY_TYPE)
	val = build_array_copy (val);
      else if (DECL_NORMAL_CAPTURE_P (field)
	       && !DECL_VLA_CAPTURE_P (field)
	       && TREE_CODE (TREE_TYPE (field)) != REFERENCE_TYPE)
	{
	  /* "the entities that are captured by copy are used to
	     direct-initialize each corresponding non-static data
	     member of the resulting closure object."

	     There's normally no way to express direct-initialization
	     from an element of a CONSTRUCTOR, so we build up a special
	     TARGET_EXPR to bypass the usual copy-initialization.  */
	  val = force_rvalue (val, tf_warning_or_error);
	  if (TREE_CODE (val) == TARGET_EXPR)
	    TARGET_EXPR_DIRECT_INIT_P (val) = true;
	}

      CONSTRUCTOR_APPEND_ELT (elts, DECL_NAME (field), val);
    }

  expr = build_constructor (init_list_type_node, elts);
  CONSTRUCTOR_IS_DIRECT_INIT (expr) = 1;

  /* N2927: "[The closure] class type is not an aggregate."
     But we briefly treat it as an aggregate to make this simpler.  */
  type = LAMBDA_EXPR_CLOSURE (lambda_expr);
  CLASSTYPE_NON_AGGREGATE (type) = 0;
  expr = finish_compound_literal (type, expr, tf_warning_or_error);
  CLASSTYPE_NON_AGGREGATE (type) = 1;

 out:
  input_location = saved_loc;
  return expr;
}

/* Return an initialized RECORD_TYPE for LAMBDA.
   LAMBDA must have its explicit captures already.  */

tree
begin_lambda_type (tree lambda)
{
  tree type;

  {
    /* Unique name.  This is just like an unnamed class, but we cannot use
       make_anon_name because of certain checks against TYPE_ANONYMOUS_P.  */
    tree name;
    name = make_lambda_name ();

    /* Create the new RECORD_TYPE for this lambda.  */
    type = xref_tag (/*tag_code=*/record_type,
                     name,
                     /*scope=*/ts_lambda,
                     /*template_header_p=*/false);
  }

  /* Designate it as a struct so that we can use aggregate initialization.  */
  CLASSTYPE_DECLARED_CLASS (type) = false;

  /* Cross-reference the expression and the type.  */
  LAMBDA_EXPR_CLOSURE (lambda) = type;
  CLASSTYPE_LAMBDA_EXPR (type) = lambda;

  /* Clear base types.  */
  xref_basetypes (type, /*bases=*/NULL_TREE);

  /* Start the class.  */
  type = begin_class_definition (type);
  if (type == error_mark_node)
    return error_mark_node;

  return type;
}

/* Returns the type to use for the return type of the operator() of a
   closure class.  */

tree
lambda_return_type (tree expr)
{
  if (expr == NULL_TREE)
    return void_type_node;
  if (type_unknown_p (expr)
      || BRACE_ENCLOSED_INITIALIZER_P (expr))
    {
      cxx_incomplete_type_error (expr, TREE_TYPE (expr));
      return void_type_node;
    }
  gcc_checking_assert (!type_dependent_expression_p (expr));
  return cv_unqualified (type_decays_to (unlowered_expr_type (expr)));
}

/* Given a LAMBDA_EXPR or closure type LAMBDA, return the op() of the
   closure type.  */

tree
lambda_function (tree lambda)
{
  tree type;
  if (TREE_CODE (lambda) == LAMBDA_EXPR)
    type = LAMBDA_EXPR_CLOSURE (lambda);
  else
    type = lambda;
  gcc_assert (LAMBDA_TYPE_P (type));
  /* Don't let debug_tree cause instantiation.  */
  if (CLASSTYPE_TEMPLATE_INSTANTIATION (type)
      && !COMPLETE_OR_OPEN_TYPE_P (type))
    return NULL_TREE;
  lambda = lookup_member (type, ansi_opname (CALL_EXPR),
			  /*protect=*/0, /*want_type=*/false,
			  tf_warning_or_error);
  if (lambda)
    lambda = BASELINK_FUNCTIONS (lambda);
  return lambda;
}

/* Returns the type to use for the FIELD_DECL corresponding to the
   capture of EXPR.
   The caller should add REFERENCE_TYPE for capture by reference.  */

tree
lambda_capture_field_type (tree expr, bool explicit_init_p)
{
  tree type;
  if (explicit_init_p)
    {
      type = make_auto ();
      type = do_auto_deduction (type, expr, type);
    }
  else
    type = non_reference (unlowered_expr_type (expr));
  if (!type || WILDCARD_TYPE_P (type) || type_uses_auto (type))
    {
      type = cxx_make_type (DECLTYPE_TYPE);
      DECLTYPE_TYPE_EXPR (type) = expr;
      DECLTYPE_FOR_LAMBDA_CAPTURE (type) = true;
      DECLTYPE_FOR_INIT_CAPTURE (type) = explicit_init_p;
      SET_TYPE_STRUCTURAL_EQUALITY (type);
    }
  return type;
}

/* Returns true iff DECL is a lambda capture proxy variable created by
   build_capture_proxy.  */

bool
is_capture_proxy (tree decl)
{
  return (VAR_P (decl)
	  && DECL_HAS_VALUE_EXPR_P (decl)
	  && !DECL_ANON_UNION_VAR_P (decl)
	  && LAMBDA_FUNCTION_P (DECL_CONTEXT (decl)));
}

/* Returns true iff DECL is a capture proxy for a normal capture
   (i.e. without explicit initializer).  */

bool
is_normal_capture_proxy (tree decl)
{
  if (!is_capture_proxy (decl))
    /* It's not a capture proxy.  */
    return false;

  /* It is a capture proxy, is it a normal capture?  */
  tree val = DECL_VALUE_EXPR (decl);
  if (val == error_mark_node)
    return true;

  gcc_assert (TREE_CODE (val) == COMPONENT_REF);
  val = TREE_OPERAND (val, 1);
  return DECL_NORMAL_CAPTURE_P (val);
}

/* VAR is a capture proxy created by build_capture_proxy; add it to the
   current function, which is the operator() for the appropriate lambda.  */

void
insert_capture_proxy (tree var)
{
  cp_binding_level *b;
  tree stmt_list;

  /* Put the capture proxy in the extra body block so that it won't clash
     with a later local variable.  */
  b = current_binding_level;
  for (;;)
    {
      cp_binding_level *n = b->level_chain;
      if (n->kind == sk_function_parms)
	break;
      b = n;
    }
  pushdecl_with_scope (var, b, false);

  /* And put a DECL_EXPR in the STATEMENT_LIST for the same block.  */
  var = build_stmt (DECL_SOURCE_LOCATION (var), DECL_EXPR, var);
  stmt_list = (*stmt_list_stack)[1];
  gcc_assert (stmt_list);
  append_to_statement_list_force (var, &stmt_list);
}

/* We've just finished processing a lambda; if the containing scope is also
   a lambda, insert any capture proxies that were created while processing
   the nested lambda.  */

void
insert_pending_capture_proxies (void)
{
  tree lam;
  vec<tree, va_gc> *proxies;
  unsigned i;

  if (!current_function_decl || !LAMBDA_FUNCTION_P (current_function_decl))
    return;

  lam = CLASSTYPE_LAMBDA_EXPR (DECL_CONTEXT (current_function_decl));
  proxies = LAMBDA_EXPR_PENDING_PROXIES (lam);
  for (i = 0; i < vec_safe_length (proxies); ++i)
    {
      tree var = (*proxies)[i];
      insert_capture_proxy (var);
    }
  release_tree_vector (LAMBDA_EXPR_PENDING_PROXIES (lam));
  LAMBDA_EXPR_PENDING_PROXIES (lam) = NULL;
}

/* Given REF, a COMPONENT_REF designating a field in the lambda closure,
   return the type we want the proxy to have: the type of the field itself,
   with added const-qualification if the lambda isn't mutable and the
   capture is by value.  */

tree
lambda_proxy_type (tree ref)
{
  tree type;
  if (REFERENCE_REF_P (ref))
    ref = TREE_OPERAND (ref, 0);
  type = TREE_TYPE (ref);
  if (type && !WILDCARD_TYPE_P (non_reference (type)))
    return type;
  type = cxx_make_type (DECLTYPE_TYPE);
  DECLTYPE_TYPE_EXPR (type) = ref;
  DECLTYPE_FOR_LAMBDA_PROXY (type) = true;
  SET_TYPE_STRUCTURAL_EQUALITY (type);
  return type;
}

/* MEMBER is a capture field in a lambda closure class.  Now that we're
   inside the operator(), build a placeholder var for future lookups and
   debugging.  */

tree
build_capture_proxy (tree member)
{
  tree var, object, fn, closure, name, lam, type;

  closure = DECL_CONTEXT (member);
  fn = lambda_function (closure);
  lam = CLASSTYPE_LAMBDA_EXPR (closure);

  /* The proxy variable forwards to the capture field.  */
  object = build_fold_indirect_ref (DECL_ARGUMENTS (fn));
  object = finish_non_static_data_member (member, object, NULL_TREE);
  if (REFERENCE_REF_P (object))
    object = TREE_OPERAND (object, 0);

  /* Remove the __ inserted by add_capture.  */
  if (DECL_NORMAL_CAPTURE_P (member))
    name = get_identifier (IDENTIFIER_POINTER (DECL_NAME (member)) + 2);
  else
    name = DECL_NAME (member);

  type = lambda_proxy_type (object);

  if (DECL_VLA_CAPTURE_P (member))
    {
      /* Rebuild the VLA type from the pointer and maxindex.  */
      tree field = next_initializable_field (TYPE_FIELDS (type));
      tree ptr = build_simple_component_ref (object, field);
      field = next_initializable_field (DECL_CHAIN (field));
      tree max = build_simple_component_ref (object, field);
      type = build_array_type (TREE_TYPE (TREE_TYPE (ptr)),
			       build_index_type (max));
      type = build_reference_type (type);
      REFERENCE_VLA_OK (type) = true;
      object = convert (type, ptr);
    }

  var = build_decl (input_location, VAR_DECL, name, type);
  SET_DECL_VALUE_EXPR (var, object);
  DECL_HAS_VALUE_EXPR_P (var) = 1;
  DECL_ARTIFICIAL (var) = 1;
  TREE_USED (var) = 1;
  DECL_CONTEXT (var) = fn;

  if (name == this_identifier)
    {
      gcc_assert (LAMBDA_EXPR_THIS_CAPTURE (lam) == member);
      LAMBDA_EXPR_THIS_CAPTURE (lam) = var;
    }

  if (fn == current_function_decl)
    insert_capture_proxy (var);
  else
    vec_safe_push (LAMBDA_EXPR_PENDING_PROXIES (lam), var);

  return var;
}

/* Return a struct containing a pointer and a length for lambda capture of
   an array of runtime length.  */

static tree
vla_capture_type (tree array_type)
{
  static tree ptr_id, max_id;
  tree type = xref_tag (record_type, make_anon_name (), ts_current, false);
  xref_basetypes (type, NULL_TREE);
  type = begin_class_definition (type);
  if (!ptr_id)
    {
      ptr_id = get_identifier ("ptr");
      max_id = get_identifier ("max");
    }
  tree ptrtype = build_pointer_type (TREE_TYPE (array_type));
  tree field = build_decl (input_location, FIELD_DECL, ptr_id, ptrtype);
  finish_member_declaration (field);
  field = build_decl (input_location, FIELD_DECL, max_id, sizetype);
  finish_member_declaration (field);
  return finish_struct (type, NULL_TREE);
}

/* From an ID and INITIALIZER, create a capture (by reference if
   BY_REFERENCE_P is true), add it to the capture-list for LAMBDA,
   and return it.  */

tree
add_capture (tree lambda, tree id, tree initializer, bool by_reference_p,
	     bool explicit_init_p)
{
  char *buf;
  tree type, member, name;
  bool vla = false;

  if (TREE_CODE (initializer) == TREE_LIST)
    initializer = build_x_compound_expr_from_list (initializer, ELK_INIT,
						   tf_warning_or_error);
  type = lambda_capture_field_type (initializer, explicit_init_p);
  if (array_of_runtime_bound_p (type))
    {
      vla = true;
      if (!by_reference_p)
	error ("array of runtime bound cannot be captured by copy, "
	       "only by reference");

      /* For a VLA, we capture the address of the first element and the
	 maximum index, and then reconstruct the VLA for the proxy.  */
      tree elt = cp_build_array_ref (input_location, initializer,
				     integer_zero_node, tf_warning_or_error);
      initializer = build_constructor_va (init_list_type_node, 2,
					  NULL_TREE, build_address (elt),
					  NULL_TREE, array_type_nelts (type));
      type = vla_capture_type (type);
    }
  else if (variably_modified_type_p (type, NULL_TREE))
    {
      error ("capture of variable-size type %qT that is not a C++1y array "
	     "of runtime bound", type);
      if (TREE_CODE (type) == ARRAY_TYPE
	  && variably_modified_type_p (TREE_TYPE (type), NULL_TREE))
	inform (input_location, "because the array element type %qT has "
		"variable size", TREE_TYPE (type));
      type = error_mark_node;
    }
  else if (by_reference_p)
    {
      type = build_reference_type (type);
      if (!real_lvalue_p (initializer))
	error ("cannot capture %qE by reference", initializer);
    }
  else
    /* Capture by copy requires a complete type.  */
    type = complete_type (type);

  /* Add __ to the beginning of the field name so that user code
     won't find the field with name lookup.  We can't just leave the name
     unset because template instantiation uses the name to find
     instantiated fields.  */
  if (!explicit_init_p)
    {
      buf = (char *) alloca (IDENTIFIER_LENGTH (id) + 3);
      buf[1] = buf[0] = '_';
      memcpy (buf + 2, IDENTIFIER_POINTER (id),
	      IDENTIFIER_LENGTH (id) + 1);
      name = get_identifier (buf);
    }
  else
    /* But captures with explicit initializers are named.  */
    name = id;

  /* If TREE_TYPE isn't set, we're still in the introducer, so check
     for duplicates.  */
  if (!LAMBDA_EXPR_CLOSURE (lambda))
    {
      if (IDENTIFIER_MARKED (name))
	{
	  pedwarn (input_location, 0,
		   "already captured %qD in lambda expression", id);
	  return NULL_TREE;
	}
      IDENTIFIER_MARKED (name) = true;
    }

  /* Make member variable.  */
  member = build_lang_decl (FIELD_DECL, name, type);
  DECL_VLA_CAPTURE_P (member) = vla;

  if (!explicit_init_p)
    /* Normal captures are invisible to name lookup but uses are replaced
       with references to the capture field; we implement this by only
       really making them invisible in unevaluated context; see
       qualify_lookup.  For now, let's make explicitly initialized captures
       always visible.  */
    DECL_NORMAL_CAPTURE_P (member) = true;

  if (id == this_identifier)
    LAMBDA_EXPR_THIS_CAPTURE (lambda) = member;

  /* Add it to the appropriate closure class if we've started it.  */
  if (current_class_type
      && current_class_type == LAMBDA_EXPR_CLOSURE (lambda))
    finish_member_declaration (member);

  LAMBDA_EXPR_CAPTURE_LIST (lambda)
    = tree_cons (member, initializer, LAMBDA_EXPR_CAPTURE_LIST (lambda));

  if (LAMBDA_EXPR_CLOSURE (lambda))
    return build_capture_proxy (member);
  /* For explicit captures we haven't started the function yet, so we wait
     and build the proxy from cp_parser_lambda_body.  */
  return NULL_TREE;
}

/* Register all the capture members on the list CAPTURES, which is the
   LAMBDA_EXPR_CAPTURE_LIST for the lambda after the introducer.  */

void
register_capture_members (tree captures)
{
  if (captures == NULL_TREE)
    return;

  register_capture_members (TREE_CHAIN (captures));
  /* We set this in add_capture to avoid duplicates.  */
  IDENTIFIER_MARKED (DECL_NAME (TREE_PURPOSE (captures))) = false;
  finish_member_declaration (TREE_PURPOSE (captures));
}

/* Similar to add_capture, except this works on a stack of nested lambdas.
   BY_REFERENCE_P in this case is derived from the default capture mode.
   Returns the capture for the lambda at the bottom of the stack.  */

tree
add_default_capture (tree lambda_stack, tree id, tree initializer)
{
  bool this_capture_p = (id == this_identifier);

  tree var = NULL_TREE;

  tree saved_class_type = current_class_type;

  tree node;

  for (node = lambda_stack;
       node;
       node = TREE_CHAIN (node))
    {
      tree lambda = TREE_VALUE (node);

      current_class_type = LAMBDA_EXPR_CLOSURE (lambda);
      var = add_capture (lambda,
                            id,
                            initializer,
                            /*by_reference_p=*/
			    (!this_capture_p
			     && (LAMBDA_EXPR_DEFAULT_CAPTURE_MODE (lambda)
				 == CPLD_REFERENCE)),
			    /*explicit_init_p=*/false);
      initializer = convert_from_reference (var);
    }

  current_class_type = saved_class_type;

  return var;
}

/* Return the capture pertaining to a use of 'this' in LAMBDA, in the form of an
   INDIRECT_REF, possibly adding it through default capturing.  */

tree
lambda_expr_this_capture (tree lambda)
{
  tree result;

  tree this_capture = LAMBDA_EXPR_THIS_CAPTURE (lambda);

  /* In unevaluated context this isn't an odr-use, so just return the
     nearest 'this'.  */
  if (cp_unevaluated_operand)
    return lookup_name (this_identifier);

  /* Try to default capture 'this' if we can.  */
  if (!this_capture
      && LAMBDA_EXPR_DEFAULT_CAPTURE_MODE (lambda) != CPLD_NONE)
    {
      tree lambda_stack = NULL_TREE;
      tree init = NULL_TREE;

      /* If we are in a lambda function, we can move out until we hit:
           1. a non-lambda function or NSDMI,
           2. a lambda function capturing 'this', or
           3. a non-default capturing lambda function.  */
      for (tree tlambda = lambda; ;)
	{
          lambda_stack = tree_cons (NULL_TREE,
                                    tlambda,
                                    lambda_stack);

	  if (LAMBDA_EXPR_EXTRA_SCOPE (tlambda)
	      && TREE_CODE (LAMBDA_EXPR_EXTRA_SCOPE (tlambda)) == FIELD_DECL)
	    {
	      /* In an NSDMI, we don't have a function to look up the decl in,
		 but the fake 'this' pointer that we're using for parsing is
		 in scope_chain.  */
	      init = scope_chain->x_current_class_ptr;
	      gcc_checking_assert
		(init && (TREE_TYPE (TREE_TYPE (init))
			  == current_nonlambda_class_type ()));
	      break;
	    }

	  tree closure_decl = TYPE_NAME (LAMBDA_EXPR_CLOSURE (tlambda));
	  tree containing_function = decl_function_context (closure_decl);

	  if (containing_function == NULL_TREE)
	    /* We ran out of scopes; there's no 'this' to capture.  */
	    break;

	  if (!LAMBDA_FUNCTION_P (containing_function))
	    {
	      /* We found a non-lambda function.  */
	      if (DECL_NONSTATIC_MEMBER_FUNCTION_P (containing_function))
		/* First parameter is 'this'.  */
		init = DECL_ARGUMENTS (containing_function);
	      break;
	    }

	  tlambda
            = CLASSTYPE_LAMBDA_EXPR (DECL_CONTEXT (containing_function));

          if (LAMBDA_EXPR_THIS_CAPTURE (tlambda))
	    {
	      /* An outer lambda has already captured 'this'.  */
	      init = LAMBDA_EXPR_THIS_CAPTURE (tlambda);
	      break;
	    }

	  if (LAMBDA_EXPR_DEFAULT_CAPTURE_MODE (tlambda) == CPLD_NONE)
	    /* An outer lambda won't let us capture 'this'.  */
	    break;
	}

      if (init)
	this_capture = add_default_capture (lambda_stack,
					    /*id=*/this_identifier,
					    init);
    }

  if (!this_capture)
    {
      error ("%<this%> was not captured for this lambda function");
      result = error_mark_node;
    }
  else
    {
      /* To make sure that current_class_ref is for the lambda.  */
      gcc_assert (TYPE_MAIN_VARIANT (TREE_TYPE (current_class_ref))
		  == LAMBDA_EXPR_CLOSURE (lambda));

      result = this_capture;

      /* If 'this' is captured, each use of 'this' is transformed into an
	 access to the corresponding unnamed data member of the closure
	 type cast (_expr.cast_ 5.4) to the type of 'this'. [ The cast
	 ensures that the transformed expression is an rvalue. ] */
      result = rvalue (result);
    }

  return result;
}

/* We don't want to capture 'this' until we know we need it, i.e. after
   overload resolution has chosen a non-static member function.  At that
   point we call this function to turn a dummy object into a use of the
   'this' capture.  */

tree
maybe_resolve_dummy (tree object)
{
  if (!is_dummy_object (object))
    return object;

  tree type = TYPE_MAIN_VARIANT (TREE_TYPE (object));
  gcc_assert (!TYPE_PTR_P (type));

  if (type != current_class_type
      && current_class_type
      && LAMBDA_TYPE_P (current_class_type)
      && DERIVED_FROM_P (type, current_nonlambda_class_type ()))
    {
      /* In a lambda, need to go through 'this' capture.  */
      tree lam = CLASSTYPE_LAMBDA_EXPR (current_class_type);
      tree cap = lambda_expr_this_capture (lam);
      object = build_x_indirect_ref (EXPR_LOCATION (object), cap,
				     RO_NULL, tf_warning_or_error);
    }

  return object;
}

/* Returns the method basetype of the innermost non-lambda function, or
   NULL_TREE if none.  */

tree
nonlambda_method_basetype (void)
{
  tree fn, type;
  if (!current_class_ref)
    return NULL_TREE;

  type = current_class_type;
  if (!LAMBDA_TYPE_P (type))
    return type;

  /* Find the nearest enclosing non-lambda function.  */
  fn = TYPE_NAME (type);
  do
    fn = decl_function_context (fn);
  while (fn && LAMBDA_FUNCTION_P (fn));

  if (!fn || !DECL_NONSTATIC_MEMBER_FUNCTION_P (fn))
    return NULL_TREE;

  return TYPE_METHOD_BASETYPE (TREE_TYPE (fn));
}

/* If the closure TYPE has a static op(), also add a conversion to function
   pointer.  */

void
maybe_add_lambda_conv_op (tree type)
{
  bool nested = (current_function_decl != NULL_TREE);
  tree callop = lambda_function (type);
  tree rettype, name, fntype, fn, body, compound_stmt;
  tree thistype, stattype, statfn, convfn, call, arg;
  vec<tree, va_gc> *argvec;

  if (LAMBDA_EXPR_CAPTURE_LIST (CLASSTYPE_LAMBDA_EXPR (type)) != NULL_TREE)
    return;

  if (processing_template_decl)
    return;

  if (DECL_INITIAL (callop) == NULL_TREE)
    {
      /* If the op() wasn't instantiated due to errors, give up.  */
      gcc_assert (errorcount || sorrycount);
      return;
    }

  stattype = build_function_type (TREE_TYPE (TREE_TYPE (callop)),
				  FUNCTION_ARG_CHAIN (callop));

  /* First build up the conversion op.  */

  rettype = build_pointer_type (stattype);
  name = mangle_conv_op_name_for_type (rettype);
  thistype = cp_build_qualified_type (type, TYPE_QUAL_CONST);
  fntype = build_method_type_directly (thistype, rettype, void_list_node);
  fn = convfn = build_lang_decl (FUNCTION_DECL, name, fntype);
  DECL_SOURCE_LOCATION (fn) = DECL_SOURCE_LOCATION (callop);

  if (TARGET_PTRMEMFUNC_VBIT_LOCATION == ptrmemfunc_vbit_in_pfn
      && DECL_ALIGN (fn) < 2 * BITS_PER_UNIT)
    DECL_ALIGN (fn) = 2 * BITS_PER_UNIT;

  SET_OVERLOADED_OPERATOR_CODE (fn, TYPE_EXPR);
  grokclassfn (type, fn, NO_SPECIAL);
  set_linkage_according_to_type (type, fn);
  rest_of_decl_compilation (fn, toplevel_bindings_p (), at_eof);
  DECL_IN_AGGR_P (fn) = 1;
  DECL_ARTIFICIAL (fn) = 1;
  DECL_NOT_REALLY_EXTERN (fn) = 1;
  DECL_DECLARED_INLINE_P (fn) = 1;
  DECL_ARGUMENTS (fn) = build_this_parm (fntype, TYPE_QUAL_CONST);
  if (nested)
    DECL_INTERFACE_KNOWN (fn) = 1;

  add_method (type, fn, NULL_TREE);

  /* Generic thunk code fails for varargs; we'll complain in mark_used if
     the conversion op is used.  */
  if (varargs_function_p (callop))
    {
      DECL_DELETED_FN (fn) = 1;
      return;
    }

  /* Now build up the thunk to be returned.  */

  name = get_identifier ("_FUN");
  fn = statfn = build_lang_decl (FUNCTION_DECL, name, stattype);
  DECL_SOURCE_LOCATION (fn) = DECL_SOURCE_LOCATION (callop);
  if (TARGET_PTRMEMFUNC_VBIT_LOCATION == ptrmemfunc_vbit_in_pfn
      && DECL_ALIGN (fn) < 2 * BITS_PER_UNIT)
    DECL_ALIGN (fn) = 2 * BITS_PER_UNIT;
  grokclassfn (type, fn, NO_SPECIAL);
  set_linkage_according_to_type (type, fn);
  rest_of_decl_compilation (fn, toplevel_bindings_p (), at_eof);
  DECL_IN_AGGR_P (fn) = 1;
  DECL_ARTIFICIAL (fn) = 1;
  DECL_NOT_REALLY_EXTERN (fn) = 1;
  DECL_DECLARED_INLINE_P (fn) = 1;
  DECL_STATIC_FUNCTION_P (fn) = 1;
  DECL_ARGUMENTS (fn) = copy_list (DECL_CHAIN (DECL_ARGUMENTS (callop)));
  for (arg = DECL_ARGUMENTS (fn); arg; arg = DECL_CHAIN (arg))
    {
      /* Avoid duplicate -Wshadow warnings.  */
      DECL_NAME (arg) = NULL_TREE;
      DECL_CONTEXT (arg) = fn;
    }
  if (nested)
    DECL_INTERFACE_KNOWN (fn) = 1;

  add_method (type, fn, NULL_TREE);

  if (nested)
    push_function_context ();
  else
    /* Still increment function_depth so that we don't GC in the
       middle of an expression.  */
    ++function_depth;

  /* Generate the body of the thunk.  */

  start_preparsed_function (statfn, NULL_TREE,
			    SF_PRE_PARSED | SF_INCLASS_INLINE);
  if (DECL_ONE_ONLY (statfn))
    {
      /* Put the thunk in the same comdat group as the call op.  */
      symtab_add_to_same_comdat_group
	 ((symtab_node) cgraph_get_create_node (statfn),
          (symtab_node) cgraph_get_create_node (callop));
    }
  body = begin_function_body ();
  compound_stmt = begin_compound_stmt (0);

  arg = build1 (NOP_EXPR, TREE_TYPE (DECL_ARGUMENTS (callop)),
		null_pointer_node);
  argvec = make_tree_vector ();
  argvec->quick_push (arg);
  for (arg = DECL_ARGUMENTS (statfn); arg; arg = DECL_CHAIN (arg))
    {
      mark_exp_read (arg);
      vec_safe_push (argvec, arg);
    }
  call = build_call_a (callop, argvec->length (), argvec->address ());
  CALL_FROM_THUNK_P (call) = 1;
  if (MAYBE_CLASS_TYPE_P (TREE_TYPE (call)))
    call = build_cplus_new (TREE_TYPE (call), call, tf_warning_or_error);
  call = convert_from_reference (call);
  finish_return_stmt (call);

  finish_compound_stmt (compound_stmt);
  finish_function_body (body);

  expand_or_defer_fn (finish_function (2));

  /* Generate the body of the conversion op.  */

  start_preparsed_function (convfn, NULL_TREE,
			    SF_PRE_PARSED | SF_INCLASS_INLINE);
  body = begin_function_body ();
  compound_stmt = begin_compound_stmt (0);

  /* decl_needed_p needs to see that it's used.  */
  TREE_USED (statfn) = 1;
  finish_return_stmt (decay_conversion (statfn, tf_warning_or_error));

  finish_compound_stmt (compound_stmt);
  finish_function_body (body);

  expand_or_defer_fn (finish_function (2));

  if (nested)
    pop_function_context ();
  else
    --function_depth;
}

/* Returns true iff VAL is a lambda-related declaration which should
   be ignored by unqualified lookup.  */

bool
is_lambda_ignored_entity (tree val)
{
  /* In unevaluated context, look past normal capture proxies.  */
  if (cp_unevaluated_operand && is_normal_capture_proxy (val))
    return true;

  /* Always ignore lambda fields, their names are only for debugging.  */
  if (TREE_CODE (val) == FIELD_DECL
      && CLASSTYPE_LAMBDA_EXPR (DECL_CONTEXT (val)))
    return true;

  /* None of the lookups that use qualify_lookup want the op() from the
     lambda; they want the one from the enclosing class.  */
  if (TREE_CODE (val) == FUNCTION_DECL && LAMBDA_FUNCTION_P (val))
    return true;

  return false;
}
