/* Tree based points-to analysis
   Copyright (C) 2005-2013 Free Software Foundation, Inc.
   Contributed by Daniel Berlin <dberlin@dberlin.org>

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   GCC is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "ggc.h"
#include "obstack.h"
#include "bitmap.h"
#include "flags.h"
#include "basic-block.h"
#include "tree.h"
#include "tree-flow.h"
#include "tree-inline.h"
#include "diagnostic-core.h"
#include "gimple.h"
#include "hash-table.h"
#include "function.h"
#include "cgraph.h"
#include "tree-pass.h"
#include "alloc-pool.h"
#include "splay-tree.h"
#include "params.h"
#include "cgraph.h"
#include "alias.h"
#include "pointer-set.h"

/* The idea behind this analyzer is to generate set constraints from the
   program, then solve the resulting constraints in order to generate the
   points-to sets.

   Set constraints are a way of modeling program analysis problems that
   involve sets.  They consist of an inclusion constraint language,
   describing the variables (each variable is a set) and operations that
   are involved on the variables, and a set of rules that derive facts
   from these operations.  To solve a system of set constraints, you derive
   all possible facts under the rules, which gives you the correct sets
   as a consequence.

   See  "Efficient Field-sensitive pointer analysis for C" by "David
   J. Pearce and Paul H. J. Kelly and Chris Hankin, at
   http://citeseer.ist.psu.edu/pearce04efficient.html

   Also see "Ultra-fast Aliasing Analysis using CLA: A Million Lines
   of C Code in a Second" by ""Nevin Heintze and Olivier Tardieu" at
   http://citeseer.ist.psu.edu/heintze01ultrafast.html

   There are three types of real constraint expressions, DEREF,
   ADDRESSOF, and SCALAR.  Each constraint expression consists
   of a constraint type, a variable, and an offset.

   SCALAR is a constraint expression type used to represent x, whether
   it appears on the LHS or the RHS of a statement.
   DEREF is a constraint expression type used to represent *x, whether
   it appears on the LHS or the RHS of a statement.
   ADDRESSOF is a constraint expression used to represent &x, whether
   it appears on the LHS or the RHS of a statement.

   Each pointer variable in the program is assigned an integer id, and
   each field of a structure variable is assigned an integer id as well.

   Structure variables are linked to their list of fields through a "next
   field" in each variable that points to the next field in offset
   order.
   Each variable for a structure field has

   1. "size", that tells the size in bits of that field.
   2. "fullsize, that tells the size in bits of the entire structure.
   3. "offset", that tells the offset in bits from the beginning of the
   structure to this field.

   Thus,
   struct f
   {
     int a;
     int b;
   } foo;
   int *bar;

   looks like

   foo.a -> id 1, size 32, offset 0, fullsize 64, next foo.b
   foo.b -> id 2, size 32, offset 32, fullsize 64, next NULL
   bar -> id 3, size 32, offset 0, fullsize 32, next NULL


  In order to solve the system of set constraints, the following is
  done:

  1. Each constraint variable x has a solution set associated with it,
  Sol(x).

  2. Constraints are separated into direct, copy, and complex.
  Direct constraints are ADDRESSOF constraints that require no extra
  processing, such as P = &Q
  Copy constraints are those of the form P = Q.
  Complex constraints are all the constraints involving dereferences
  and offsets (including offsetted copies).

  3. All direct constraints of the form P = &Q are processed, such
  that Q is added to Sol(P)

  4. All complex constraints for a given constraint variable are stored in a
  linked list attached to that variable's node.

  5. A directed graph is built out of the copy constraints. Each
  constraint variable is a node in the graph, and an edge from
  Q to P is added for each copy constraint of the form P = Q

  6. The graph is then walked, and solution sets are
  propagated along the copy edges, such that an edge from Q to P
  causes Sol(P) <- Sol(P) union Sol(Q).

  7.  As we visit each node, all complex constraints associated with
  that node are processed by adding appropriate copy edges to the graph, or the
  appropriate variables to the solution set.

  8. The process of walking the graph is iterated until no solution
  sets change.

  Prior to walking the graph in steps 6 and 7, We perform static
  cycle elimination on the constraint graph, as well
  as off-line variable substitution.

  TODO: Adding offsets to pointer-to-structures can be handled (IE not punted
  on and turned into anything), but isn't.  You can just see what offset
  inside the pointed-to struct it's going to access.

  TODO: Constant bounded arrays can be handled as if they were structs of the
  same number of elements.

  TODO: Modeling heap and incoming pointers becomes much better if we
  add fields to them as we discover them, which we could do.

  TODO: We could handle unions, but to be honest, it's probably not
  worth the pain or slowdown.  */

/* IPA-PTA optimizations possible.

   When the indirect function called is ANYTHING we can add disambiguation
   based on the function signatures (or simply the parameter count which
   is the varinfo size).  We also do not need to consider functions that
   do not have their address taken.

   The is_global_var bit which marks escape points is overly conservative
   in IPA mode.  Split it to is_escape_point and is_global_var - only
   externally visible globals are escape points in IPA mode.  This is
   also needed to fix the pt_solution_includes_global predicate
   (and thus ptr_deref_may_alias_global_p).

   The way we introduce DECL_PT_UID to avoid fixing up all points-to
   sets in the translation unit when we copy a DECL during inlining
   pessimizes precision.  The advantage is that the DECL_PT_UID keeps
   compile-time and memory usage overhead low - the points-to sets
   do not grow or get unshared as they would during a fixup phase.
   An alternative solution is to delay IPA PTA until after all
   inlining transformations have been applied.

   The way we propagate clobber/use information isn't optimized.
   It should use a new complex constraint that properly filters
   out local variables of the callee (though that would make
   the sets invalid after inlining).  OTOH we might as well
   admit defeat to WHOPR and simply do all the clobber/use analysis
   and propagation after PTA finished but before we threw away
   points-to information for memory variables.  WHOPR and PTA
   do not play along well anyway - the whole constraint solving
   would need to be done in WPA phase and it will be very interesting
   to apply the results to local SSA names during LTRANS phase.

   We probably should compute a per-function unit-ESCAPE solution
   propagating it simply like the clobber / uses solutions.  The
   solution can go alongside the non-IPA espaced solution and be
   used to query which vars escape the unit through a function.

   We never put function decls in points-to sets so we do not
   keep the set of called functions for indirect calls.

   And probably more.  */

static bool use_field_sensitive = true;
static int in_ipa_mode = 0;

/* Used for predecessor bitmaps. */
static bitmap_obstack predbitmap_obstack;

/* Used for points-to sets.  */
static bitmap_obstack pta_obstack;

/* Used for oldsolution members of variables. */
static bitmap_obstack oldpta_obstack;

/* Used for per-solver-iteration bitmaps.  */
static bitmap_obstack iteration_obstack;

static unsigned int create_variable_info_for (tree, const char *);
typedef struct constraint_graph *constraint_graph_t;
static void unify_nodes (constraint_graph_t, unsigned int, unsigned int, bool);

struct constraint;
typedef struct constraint *constraint_t;


#define EXECUTE_IF_IN_NONNULL_BITMAP(a, b, c, d)	\
  if (a)						\
    EXECUTE_IF_SET_IN_BITMAP (a, b, c, d)

static struct constraint_stats
{
  unsigned int total_vars;
  unsigned int nonpointer_vars;
  unsigned int unified_vars_static;
  unsigned int unified_vars_dynamic;
  unsigned int iterations;
  unsigned int num_edges;
  unsigned int num_implicit_edges;
  unsigned int points_to_sets_created;
} stats;

struct variable_info
{
  /* ID of this variable  */
  unsigned int id;

  /* True if this is a variable created by the constraint analysis, such as
     heap variables and constraints we had to break up.  */
  unsigned int is_artificial_var : 1;

  /* True if this is a special variable whose solution set should not be
     changed.  */
  unsigned int is_special_var : 1;

  /* True for variables whose size is not known or variable.  */
  unsigned int is_unknown_size_var : 1;

  /* True for (sub-)fields that represent a whole variable.  */
  unsigned int is_full_var : 1;

  /* True if this is a heap variable.  */
  unsigned int is_heap_var : 1;

  /* True if this field may contain pointers.  */
  unsigned int may_have_pointers : 1;

  /* True if this field has only restrict qualified pointers.  */
  unsigned int only_restrict_pointers : 1;

  /* True if this represents a global variable.  */
  unsigned int is_global_var : 1;

  /* True if this represents a IPA function info.  */
  unsigned int is_fn_info : 1;

  /* The ID of the variable for the next field in this structure
     or zero for the last field in this structure.  */
  unsigned next;

  /* The ID of the variable for the first field in this structure.  */
  unsigned head;

  /* Offset of this variable, in bits, from the base variable  */
  unsigned HOST_WIDE_INT offset;

  /* Size of the variable, in bits.  */
  unsigned HOST_WIDE_INT size;

  /* Full size of the base variable, in bits.  */
  unsigned HOST_WIDE_INT fullsize;

  /* Name of this variable */
  const char *name;

  /* Tree that this variable is associated with.  */
  tree decl;

  /* Points-to set for this variable.  */
  bitmap solution;

  /* Old points-to set for this variable.  */
  bitmap oldsolution;
};
typedef struct variable_info *varinfo_t;

static varinfo_t first_vi_for_offset (varinfo_t, unsigned HOST_WIDE_INT);
static varinfo_t first_or_preceding_vi_for_offset (varinfo_t,
						   unsigned HOST_WIDE_INT);
static varinfo_t lookup_vi_for_tree (tree);
static inline bool type_can_have_subvars (const_tree);

/* Pool of variable info structures.  */
static alloc_pool variable_info_pool;

/* Map varinfo to final pt_solution.  */
static pointer_map_t *final_solutions;
struct obstack final_solutions_obstack;

/* Table of variable info structures for constraint variables.
   Indexed directly by variable info id.  */
static vec<varinfo_t> varmap;

/* Return the varmap element N */

static inline varinfo_t
get_varinfo (unsigned int n)
{
  return varmap[n];
}

/* Return the next variable in the list of sub-variables of VI
   or NULL if VI is the last sub-variable.  */

static inline varinfo_t
vi_next (varinfo_t vi)
{
  return get_varinfo (vi->next);
}

/* Static IDs for the special variables.  Variable ID zero is unused
   and used as terminator for the sub-variable chain.  */
enum { nothing_id = 1, anything_id = 2, readonly_id = 3,
       escaped_id = 4, nonlocal_id = 5,
       storedanything_id = 6, integer_id = 7 };

/* Return a new variable info structure consisting for a variable
   named NAME, and using constraint graph node NODE.  Append it
   to the vector of variable info structures.  */

static varinfo_t
new_var_info (tree t, const char *name)
{
  unsigned index = varmap.length ();
  varinfo_t ret = (varinfo_t) pool_alloc (variable_info_pool);

  ret->id = index;
  ret->name = name;
  ret->decl = t;
  /* Vars without decl are artificial and do not have sub-variables.  */
  ret->is_artificial_var = (t == NULL_TREE);
  ret->is_special_var = false;
  ret->is_unknown_size_var = false;
  ret->is_full_var = (t == NULL_TREE);
  ret->is_heap_var = false;
  ret->may_have_pointers = true;
  ret->only_restrict_pointers = false;
  ret->is_global_var = (t == NULL_TREE);
  ret->is_fn_info = false;
  if (t && DECL_P (t))
    ret->is_global_var = (is_global_var (t)
			  /* We have to treat even local register variables
			     as escape points.  */
			  || (TREE_CODE (t) == VAR_DECL
			      && DECL_HARD_REGISTER (t)));
  ret->solution = BITMAP_ALLOC (&pta_obstack);
  ret->oldsolution = NULL;
  ret->next = 0;
  ret->head = ret->id;

  stats.total_vars++;

  varmap.safe_push (ret);

  return ret;
}


/* A map mapping call statements to per-stmt variables for uses
   and clobbers specific to the call.  */
static struct pointer_map_t *call_stmt_vars;

/* Lookup or create the variable for the call statement CALL.  */

static varinfo_t
get_call_vi (gimple call)
{
  void **slot_p;
  varinfo_t vi, vi2;

  slot_p = pointer_map_insert (call_stmt_vars, call);
  if (*slot_p)
    return (varinfo_t) *slot_p;

  vi = new_var_info (NULL_TREE, "CALLUSED");
  vi->offset = 0;
  vi->size = 1;
  vi->fullsize = 2;
  vi->is_full_var = true;

  vi2 = new_var_info (NULL_TREE, "CALLCLOBBERED");
  vi2->offset = 1;
  vi2->size = 1;
  vi2->fullsize = 2;
  vi2->is_full_var = true;

  vi->next = vi2->id;

  *slot_p = (void *) vi;
  return vi;
}

/* Lookup the variable for the call statement CALL representing
   the uses.  Returns NULL if there is nothing special about this call.  */

static varinfo_t
lookup_call_use_vi (gimple call)
{
  void **slot_p;

  slot_p = pointer_map_contains (call_stmt_vars, call);
  if (slot_p)
    return (varinfo_t) *slot_p;

  return NULL;
}

/* Lookup the variable for the call statement CALL representing
   the clobbers.  Returns NULL if there is nothing special about this call.  */

static varinfo_t
lookup_call_clobber_vi (gimple call)
{
  varinfo_t uses = lookup_call_use_vi (call);
  if (!uses)
    return NULL;

  return vi_next (uses);
}

/* Lookup or create the variable for the call statement CALL representing
   the uses.  */

static varinfo_t
get_call_use_vi (gimple call)
{
  return get_call_vi (call);
}

/* Lookup or create the variable for the call statement CALL representing
   the clobbers.  */

static varinfo_t ATTRIBUTE_UNUSED
get_call_clobber_vi (gimple call)
{
  return vi_next (get_call_vi (call));
}


typedef enum {SCALAR, DEREF, ADDRESSOF} constraint_expr_type;

/* An expression that appears in a constraint.  */

struct constraint_expr
{
  /* Constraint type.  */
  constraint_expr_type type;

  /* Variable we are referring to in the constraint.  */
  unsigned int var;

  /* Offset, in bits, of this constraint from the beginning of
     variables it ends up referring to.

     IOW, in a deref constraint, we would deref, get the result set,
     then add OFFSET to each member.   */
  HOST_WIDE_INT offset;
};

/* Use 0x8000... as special unknown offset.  */
#define UNKNOWN_OFFSET ((HOST_WIDE_INT)-1 << (HOST_BITS_PER_WIDE_INT-1))

typedef struct constraint_expr ce_s;
static void get_constraint_for_1 (tree, vec<ce_s> *, bool, bool);
static void get_constraint_for (tree, vec<ce_s> *);
static void get_constraint_for_rhs (tree, vec<ce_s> *);
static void do_deref (vec<ce_s> *);

/* Our set constraints are made up of two constraint expressions, one
   LHS, and one RHS.

   As described in the introduction, our set constraints each represent an
   operation between set valued variables.
*/
struct constraint
{
  struct constraint_expr lhs;
  struct constraint_expr rhs;
};

/* List of constraints that we use to build the constraint graph from.  */

static vec<constraint_t> constraints;
static alloc_pool constraint_pool;

/* The constraint graph is represented as an array of bitmaps
   containing successor nodes.  */

struct constraint_graph
{
  /* Size of this graph, which may be different than the number of
     nodes in the variable map.  */
  unsigned int size;

  /* Explicit successors of each node. */
  bitmap *succs;

  /* Implicit predecessors of each node (Used for variable
     substitution). */
  bitmap *implicit_preds;

  /* Explicit predecessors of each node (Used for variable substitution).  */
  bitmap *preds;

  /* Indirect cycle representatives, or -1 if the node has no indirect
     cycles.  */
  int *indirect_cycles;

  /* Representative node for a node.  rep[a] == a unless the node has
     been unified. */
  unsigned int *rep;

  /* Equivalence class representative for a label.  This is used for
     variable substitution.  */
  int *eq_rep;

  /* Pointer equivalence label for a node.  All nodes with the same
     pointer equivalence label can be unified together at some point
     (either during constraint optimization or after the constraint
     graph is built).  */
  unsigned int *pe;

  /* Pointer equivalence representative for a label.  This is used to
     handle nodes that are pointer equivalent but not location
     equivalent.  We can unite these once the addressof constraints
     are transformed into initial points-to sets.  */
  int *pe_rep;

  /* Pointer equivalence label for each node, used during variable
     substitution.  */
  unsigned int *pointer_label;

  /* Location equivalence label for each node, used during location
     equivalence finding.  */
  unsigned int *loc_label;

  /* Pointed-by set for each node, used during location equivalence
     finding.  This is pointed-by rather than pointed-to, because it
     is constructed using the predecessor graph.  */
  bitmap *pointed_by;

  /* Points to sets for pointer equivalence.  This is *not* the actual
     points-to sets for nodes.  */
  bitmap *points_to;

  /* Bitmap of nodes where the bit is set if the node is a direct
     node.  Used for variable substitution.  */
  sbitmap direct_nodes;

  /* Bitmap of nodes where the bit is set if the node is address
     taken.  Used for variable substitution.  */
  bitmap address_taken;

  /* Vector of complex constraints for each graph node.  Complex
     constraints are those involving dereferences or offsets that are
     not 0.  */
  vec<constraint_t> *complex;
};

static constraint_graph_t graph;

/* During variable substitution and the offline version of indirect
   cycle finding, we create nodes to represent dereferences and
   address taken constraints.  These represent where these start and
   end.  */
#define FIRST_REF_NODE (varmap).length ()
#define LAST_REF_NODE (FIRST_REF_NODE + (FIRST_REF_NODE - 1))

/* Return the representative node for NODE, if NODE has been unioned
   with another NODE.
   This function performs path compression along the way to finding
   the representative.  */

static unsigned int
find (unsigned int node)
{
  gcc_checking_assert (node < graph->size);
  if (graph->rep[node] != node)
    return graph->rep[node] = find (graph->rep[node]);
  return node;
}

/* Union the TO and FROM nodes to the TO nodes.
   Note that at some point in the future, we may want to do
   union-by-rank, in which case we are going to have to return the
   node we unified to.  */

static bool
unite (unsigned int to, unsigned int from)
{
  gcc_checking_assert (to < graph->size && from < graph->size);
  if (to != from && graph->rep[from] != to)
    {
      graph->rep[from] = to;
      return true;
    }
  return false;
}

/* Create a new constraint consisting of LHS and RHS expressions.  */

static constraint_t
new_constraint (const struct constraint_expr lhs,
		const struct constraint_expr rhs)
{
  constraint_t ret = (constraint_t) pool_alloc (constraint_pool);
  ret->lhs = lhs;
  ret->rhs = rhs;
  return ret;
}

/* Print out constraint C to FILE.  */

static void
dump_constraint (FILE *file, constraint_t c)
{
  if (c->lhs.type == ADDRESSOF)
    fprintf (file, "&");
  else if (c->lhs.type == DEREF)
    fprintf (file, "*");
  fprintf (file, "%s", get_varinfo (c->lhs.var)->name);
  if (c->lhs.offset == UNKNOWN_OFFSET)
    fprintf (file, " + UNKNOWN");
  else if (c->lhs.offset != 0)
    fprintf (file, " + " HOST_WIDE_INT_PRINT_DEC, c->lhs.offset);
  fprintf (file, " = ");
  if (c->rhs.type == ADDRESSOF)
    fprintf (file, "&");
  else if (c->rhs.type == DEREF)
    fprintf (file, "*");
  fprintf (file, "%s", get_varinfo (c->rhs.var)->name);
  if (c->rhs.offset == UNKNOWN_OFFSET)
    fprintf (file, " + UNKNOWN");
  else if (c->rhs.offset != 0)
    fprintf (file, " + " HOST_WIDE_INT_PRINT_DEC, c->rhs.offset);
}


void debug_constraint (constraint_t);
void debug_constraints (void);
void debug_constraint_graph (void);
void debug_solution_for_var (unsigned int);
void debug_sa_points_to_info (void);

/* Print out constraint C to stderr.  */

DEBUG_FUNCTION void
debug_constraint (constraint_t c)
{
  dump_constraint (stderr, c);
  fprintf (stderr, "\n");
}

/* Print out all constraints to FILE */

static void
dump_constraints (FILE *file, int from)
{
  int i;
  constraint_t c;
  for (i = from; constraints.iterate (i, &c); i++)
    if (c)
      {
	dump_constraint (file, c);
	fprintf (file, "\n");
      }
}

/* Print out all constraints to stderr.  */

DEBUG_FUNCTION void
debug_constraints (void)
{
  dump_constraints (stderr, 0);
}

/* Print the constraint graph in dot format.  */

static void
dump_constraint_graph (FILE *file)
{
  unsigned int i;

  /* Only print the graph if it has already been initialized:  */
  if (!graph)
    return;

  /* Prints the header of the dot file:  */
  fprintf (file, "strict digraph {\n");
  fprintf (file, "  node [\n    shape = box\n  ]\n");
  fprintf (file, "  edge [\n    fontsize = \"12\"\n  ]\n");
  fprintf (file, "\n  // List of nodes and complex constraints in "
	   "the constraint graph:\n");

  /* The next lines print the nodes in the graph together with the
     complex constraints attached to them.  */
  for (i = 1; i < graph->size; i++)
    {
      if (i == FIRST_REF_NODE)
	continue;
      if (find (i) != i)
	continue;
      if (i < FIRST_REF_NODE)
	fprintf (file, "\"%s\"", get_varinfo (i)->name);
      else
	fprintf (file, "\"*%s\"", get_varinfo (i - FIRST_REF_NODE)->name);
      if (graph->complex[i].exists ())
	{
	  unsigned j;
	  constraint_t c;
	  fprintf (file, " [label=\"\\N\\n");
	  for (j = 0; graph->complex[i].iterate (j, &c); ++j)
	    {
	      dump_constraint (file, c);
	      fprintf (file, "\\l");
	    }
	  fprintf (file, "\"]");
	}
      fprintf (file, ";\n");
    }

  /* Go over the edges.  */
  fprintf (file, "\n  // Edges in the constraint graph:\n");
  for (i = 1; i < graph->size; i++)
    {
      unsigned j;
      bitmap_iterator bi;
      if (find (i) != i)
	continue;
      EXECUTE_IF_IN_NONNULL_BITMAP (graph->succs[i], 0, j, bi)
	{
	  unsigned to = find (j);
	  if (i == to)
	    continue;
	  if (i < FIRST_REF_NODE)
	    fprintf (file, "\"%s\"", get_varinfo (i)->name);
	  else
	    fprintf (file, "\"*%s\"", get_varinfo (i - FIRST_REF_NODE)->name);
	  fprintf (file, " -> ");
	  if (to < FIRST_REF_NODE)
	    fprintf (file, "\"%s\"", get_varinfo (to)->name);
	  else
	    fprintf (file, "\"*%s\"", get_varinfo (to - FIRST_REF_NODE)->name);
	  fprintf (file, ";\n");
	}
    }

  /* Prints the tail of the dot file.  */
  fprintf (file, "}\n");
}

/* Print out the constraint graph to stderr.  */

DEBUG_FUNCTION void
debug_constraint_graph (void)
{
  dump_constraint_graph (stderr);
}

/* SOLVER FUNCTIONS

   The solver is a simple worklist solver, that works on the following
   algorithm:

   sbitmap changed_nodes = all zeroes;
   changed_count = 0;
   For each node that is not already collapsed:
       changed_count++;
       set bit in changed nodes

   while (changed_count > 0)
   {
     compute topological ordering for constraint graph

     find and collapse cycles in the constraint graph (updating
     changed if necessary)

     for each node (n) in the graph in topological order:
       changed_count--;

       Process each complex constraint associated with the node,
       updating changed if necessary.

       For each outgoing edge from n, propagate the solution from n to
       the destination of the edge, updating changed as necessary.

   }  */

/* Return true if two constraint expressions A and B are equal.  */

static bool
constraint_expr_equal (struct constraint_expr a, struct constraint_expr b)
{
  return a.type == b.type && a.var == b.var && a.offset == b.offset;
}

/* Return true if constraint expression A is less than constraint expression
   B.  This is just arbitrary, but consistent, in order to give them an
   ordering.  */

static bool
constraint_expr_less (struct constraint_expr a, struct constraint_expr b)
{
  if (a.type == b.type)
    {
      if (a.var == b.var)
	return a.offset < b.offset;
      else
	return a.var < b.var;
    }
  else
    return a.type < b.type;
}

/* Return true if constraint A is less than constraint B.  This is just
   arbitrary, but consistent, in order to give them an ordering.  */

static bool
constraint_less (const constraint_t &a, const constraint_t &b)
{
  if (constraint_expr_less (a->lhs, b->lhs))
    return true;
  else if (constraint_expr_less (b->lhs, a->lhs))
    return false;
  else
    return constraint_expr_less (a->rhs, b->rhs);
}

/* Return true if two constraints A and B are equal.  */

static bool
constraint_equal (struct constraint a, struct constraint b)
{
  return constraint_expr_equal (a.lhs, b.lhs)
    && constraint_expr_equal (a.rhs, b.rhs);
}


/* Find a constraint LOOKFOR in the sorted constraint vector VEC */

static constraint_t
constraint_vec_find (vec<constraint_t> vec,
		     struct constraint lookfor)
{
  unsigned int place;
  constraint_t found;

  if (!vec.exists ())
    return NULL;

  place = vec.lower_bound (&lookfor, constraint_less);
  if (place >= vec.length ())
    return NULL;
  found = vec[place];
  if (!constraint_equal (*found, lookfor))
    return NULL;
  return found;
}

/* Union two constraint vectors, TO and FROM.  Put the result in TO.  */

static void
constraint_set_union (vec<constraint_t> *to,
		      vec<constraint_t> *from)
{
  int i;
  constraint_t c;

  FOR_EACH_VEC_ELT (*from, i, c)
    {
      if (constraint_vec_find (*to, *c) == NULL)
	{
	  unsigned int place = to->lower_bound (c, constraint_less);
	  to->safe_insert (place, c);
	}
    }
}

/* Expands the solution in SET to all sub-fields of variables included.  */

static void
solution_set_expand (bitmap set)
{
  bitmap_iterator bi;
  unsigned j;

  /* In a first pass expand to the head of the variables we need to
     add all sub-fields off.  This avoids quadratic behavior.  */
  EXECUTE_IF_SET_IN_BITMAP (set, 0, j, bi)
    {
      varinfo_t v = get_varinfo (j);
      if (v->is_artificial_var
	  || v->is_full_var)
	continue;
      bitmap_set_bit (set, v->head);
    }

  /* In the second pass now expand all head variables with subfields.  */
  EXECUTE_IF_SET_IN_BITMAP (set, 0, j, bi)
    {
      varinfo_t v = get_varinfo (j);
      if (v->is_artificial_var
	  || v->is_full_var
	  || v->head != j)
	continue;
      for (v = vi_next (v); v != NULL; v = vi_next (v))
	bitmap_set_bit (set, v->id);
    }
}

/* Union solution sets TO and FROM, and add INC to each member of FROM in the
   process.  */

static bool
set_union_with_increment  (bitmap to, bitmap from, HOST_WIDE_INT inc)
{
  bool changed = false;
  bitmap_iterator bi;
  unsigned int i;

  /* If the solution of FROM contains anything it is good enough to transfer
     this to TO.  */
  if (bitmap_bit_p (from, anything_id))
    return bitmap_set_bit (to, anything_id);

  /* For zero offset simply union the solution into the destination.  */
  if (inc == 0)
    return bitmap_ior_into (to, from);

  /* If the offset is unknown we have to expand the solution to
     all subfields.  */
  if (inc == UNKNOWN_OFFSET)
    {
      bitmap tmp = BITMAP_ALLOC (&iteration_obstack);
      bitmap_copy (tmp, from);
      solution_set_expand (tmp);
      changed |= bitmap_ior_into (to, tmp);
      BITMAP_FREE (tmp);
      return changed;
    }

  /* For non-zero offset union the offsetted solution into the destination.  */
  EXECUTE_IF_SET_IN_BITMAP (from, 0, i, bi)
    {
      varinfo_t vi = get_varinfo (i);

      /* If this is a variable with just one field just set its bit
         in the result.  */
      if (vi->is_artificial_var
	  || vi->is_unknown_size_var
	  || vi->is_full_var)
	changed |= bitmap_set_bit (to, i);
      else
	{
	  unsigned HOST_WIDE_INT fieldoffset = vi->offset + inc;

	  /* If the offset makes the pointer point to before the
	     variable use offset zero for the field lookup.  */
	  if (inc < 0
	      && fieldoffset > vi->offset)
	    fieldoffset = 0;

	  vi = first_or_preceding_vi_for_offset (vi, fieldoffset);

	  changed |= bitmap_set_bit (to, vi->id);
	  /* If the result is not exactly at fieldoffset include the next
	     field as well.  See get_constraint_for_ptr_offset for more
	     rationale.  */
	  if (vi->offset != fieldoffset
	      && vi->next != 0)
	    changed |= bitmap_set_bit (to, vi->next);
	}
    }

  return changed;
}

/* Insert constraint C into the list of complex constraints for graph
   node VAR.  */

static void
insert_into_complex (constraint_graph_t graph,
		     unsigned int var, constraint_t c)
{
  vec<constraint_t> complex = graph->complex[var];
  unsigned int place = complex.lower_bound (c, constraint_less);

  /* Only insert constraints that do not already exist.  */
  if (place >= complex.length ()
      || !constraint_equal (*c, *complex[place]))
    graph->complex[var].safe_insert (place, c);
}


/* Condense two variable nodes into a single variable node, by moving
   all associated info from SRC to TO.  */

static void
merge_node_constraints (constraint_graph_t graph, unsigned int to,
			unsigned int from)
{
  unsigned int i;
  constraint_t c;

  gcc_checking_assert (find (from) == to);

  /* Move all complex constraints from src node into to node  */
  FOR_EACH_VEC_ELT (graph->complex[from], i, c)
    {
      /* In complex constraints for node src, we may have either
	 a = *src, and *src = a, or an offseted constraint which are
	 always added to the rhs node's constraints.  */

      if (c->rhs.type == DEREF)
	c->rhs.var = to;
      else if (c->lhs.type == DEREF)
	c->lhs.var = to;
      else
	c->rhs.var = to;
    }
  constraint_set_union (&graph->complex[to], &graph->complex[from]);
  graph->complex[from].release ();
}


/* Remove edges involving NODE from GRAPH.  */

static void
clear_edges_for_node (constraint_graph_t graph, unsigned int node)
{
  if (graph->succs[node])
    BITMAP_FREE (graph->succs[node]);
}

/* Merge GRAPH nodes FROM and TO into node TO.  */

static void
merge_graph_nodes (constraint_graph_t graph, unsigned int to,
		   unsigned int from)
{
  if (graph->indirect_cycles[from] != -1)
    {
      /* If we have indirect cycles with the from node, and we have
	 none on the to node, the to node has indirect cycles from the
	 from node now that they are unified.
	 If indirect cycles exist on both, unify the nodes that they
	 are in a cycle with, since we know they are in a cycle with
	 each other.  */
      if (graph->indirect_cycles[to] == -1)
	graph->indirect_cycles[to] = graph->indirect_cycles[from];
    }

  /* Merge all the successor edges.  */
  if (graph->succs[from])
    {
      if (!graph->succs[to])
	graph->succs[to] = BITMAP_ALLOC (&pta_obstack);
      bitmap_ior_into (graph->succs[to],
		       graph->succs[from]);
    }

  clear_edges_for_node (graph, from);
}


/* Add an indirect graph edge to GRAPH, going from TO to FROM if
   it doesn't exist in the graph already.  */

static void
add_implicit_graph_edge (constraint_graph_t graph, unsigned int to,
			 unsigned int from)
{
  if (to == from)
    return;

  if (!graph->implicit_preds[to])
    graph->implicit_preds[to] = BITMAP_ALLOC (&predbitmap_obstack);

  if (bitmap_set_bit (graph->implicit_preds[to], from))
    stats.num_implicit_edges++;
}

/* Add a predecessor graph edge to GRAPH, going from TO to FROM if
   it doesn't exist in the graph already.
   Return false if the edge already existed, true otherwise.  */

static void
add_pred_graph_edge (constraint_graph_t graph, unsigned int to,
		     unsigned int from)
{
  if (!graph->preds[to])
    graph->preds[to] = BITMAP_ALLOC (&predbitmap_obstack);
  bitmap_set_bit (graph->preds[to], from);
}

/* Add a graph edge to GRAPH, going from FROM to TO if
   it doesn't exist in the graph already.
   Return false if the edge already existed, true otherwise.  */

static bool
add_graph_edge (constraint_graph_t graph, unsigned int to,
		unsigned int from)
{
  if (to == from)
    {
      return false;
    }
  else
    {
      bool r = false;

      if (!graph->succs[from])
	graph->succs[from] = BITMAP_ALLOC (&pta_obstack);
      if (bitmap_set_bit (graph->succs[from], to))
	{
	  r = true;
	  if (to < FIRST_REF_NODE && from < FIRST_REF_NODE)
	    stats.num_edges++;
	}
      return r;
    }
}


/* Initialize the constraint graph structure to contain SIZE nodes.  */

static void
init_graph (unsigned int size)
{
  unsigned int j;

  graph = XCNEW (struct constraint_graph);
  graph->size = size;
  graph->succs = XCNEWVEC (bitmap, graph->size);
  graph->indirect_cycles = XNEWVEC (int, graph->size);
  graph->rep = XNEWVEC (unsigned int, graph->size);
  /* ??? Macros do not support template types with multiple arguments,
     so we use a typedef to work around it.  */
  typedef vec<constraint_t> vec_constraint_t_heap;
  graph->complex = XCNEWVEC (vec_constraint_t_heap, size);
  graph->pe = XCNEWVEC (unsigned int, graph->size);
  graph->pe_rep = XNEWVEC (int, graph->size);

  for (j = 0; j < graph->size; j++)
    {
      graph->rep[j] = j;
      graph->pe_rep[j] = -1;
      graph->indirect_cycles[j] = -1;
    }
}

/* Build the constraint graph, adding only predecessor edges right now.  */

static void
build_pred_graph (void)
{
  int i;
  constraint_t c;
  unsigned int j;

  graph->implicit_preds = XCNEWVEC (bitmap, graph->size);
  graph->preds = XCNEWVEC (bitmap, graph->size);
  graph->pointer_label = XCNEWVEC (unsigned int, graph->size);
  graph->loc_label = XCNEWVEC (unsigned int, graph->size);
  graph->pointed_by = XCNEWVEC (bitmap, graph->size);
  graph->points_to = XCNEWVEC (bitmap, graph->size);
  graph->eq_rep = XNEWVEC (int, graph->size);
  graph->direct_nodes = sbitmap_alloc (graph->size);
  graph->address_taken = BITMAP_ALLOC (&predbitmap_obstack);
  bitmap_clear (graph->direct_nodes);

  for (j = 1; j < FIRST_REF_NODE; j++)
    {
      if (!get_varinfo (j)->is_special_var)
	bitmap_set_bit (graph->direct_nodes, j);
    }

  for (j = 0; j < graph->size; j++)
    graph->eq_rep[j] = -1;

  for (j = 0; j < varmap.length (); j++)
    graph->indirect_cycles[j] = -1;

  FOR_EACH_VEC_ELT (constraints, i, c)
    {
      struct constraint_expr lhs = c->lhs;
      struct constraint_expr rhs = c->rhs;
      unsigned int lhsvar = lhs.var;
      unsigned int rhsvar = rhs.var;

      if (lhs.type == DEREF)
	{
	  /* *x = y.  */
	  if (rhs.offset == 0 && lhs.offset == 0 && rhs.type == SCALAR)
	    add_pred_graph_edge (graph, FIRST_REF_NODE + lhsvar, rhsvar);
	}
      else if (rhs.type == DEREF)
	{
	  /* x = *y */
	  if (rhs.offset == 0 && lhs.offset == 0 && lhs.type == SCALAR)
	    add_pred_graph_edge (graph, lhsvar, FIRST_REF_NODE + rhsvar);
	  else
	    bitmap_clear_bit (graph->direct_nodes, lhsvar);
	}
      else if (rhs.type == ADDRESSOF)
	{
	  varinfo_t v;

	  /* x = &y */
	  if (graph->points_to[lhsvar] == NULL)
	    graph->points_to[lhsvar] = BITMAP_ALLOC (&predbitmap_obstack);
	  bitmap_set_bit (graph->points_to[lhsvar], rhsvar);

	  if (graph->pointed_by[rhsvar] == NULL)
	    graph->pointed_by[rhsvar] = BITMAP_ALLOC (&predbitmap_obstack);
	  bitmap_set_bit (graph->pointed_by[rhsvar], lhsvar);

	  /* Implicitly, *x = y */
	  add_implicit_graph_edge (graph, FIRST_REF_NODE + lhsvar, rhsvar);

	  /* All related variables are no longer direct nodes.  */
	  bitmap_clear_bit (graph->direct_nodes, rhsvar);
          v = get_varinfo (rhsvar);
          if (!v->is_full_var)
            {
              v = get_varinfo (v->head);
              do
                {
                  bitmap_clear_bit (graph->direct_nodes, v->id);
                  v = vi_next (v);
                }
              while (v != NULL);
            }
	  bitmap_set_bit (graph->address_taken, rhsvar);
	}
      else if (lhsvar > anything_id
	       && lhsvar != rhsvar && lhs.offset == 0 && rhs.offset == 0)
	{
	  /* x = y */
	  add_pred_graph_edge (graph, lhsvar, rhsvar);
	  /* Implicitly, *x = *y */
	  add_implicit_graph_edge (graph, FIRST_REF_NODE + lhsvar,
				   FIRST_REF_NODE + rhsvar);
	}
      else if (lhs.offset != 0 || rhs.offset != 0)
	{
	  if (rhs.offset != 0)
	    bitmap_clear_bit (graph->direct_nodes, lhs.var);
	  else if (lhs.offset != 0)
	    bitmap_clear_bit (graph->direct_nodes, rhs.var);
	}
    }
}

/* Build the constraint graph, adding successor edges.  */

static void
build_succ_graph (void)
{
  unsigned i, t;
  constraint_t c;

  FOR_EACH_VEC_ELT (constraints, i, c)
    {
      struct constraint_expr lhs;
      struct constraint_expr rhs;
      unsigned int lhsvar;
      unsigned int rhsvar;

      if (!c)
	continue;

      lhs = c->lhs;
      rhs = c->rhs;
      lhsvar = find (lhs.var);
      rhsvar = find (rhs.var);

      if (lhs.type == DEREF)
	{
	  if (rhs.offset == 0 && lhs.offset == 0 && rhs.type == SCALAR)
	    add_graph_edge (graph, FIRST_REF_NODE + lhsvar, rhsvar);
	}
      else if (rhs.type == DEREF)
	{
	  if (rhs.offset == 0 && lhs.offset == 0 && lhs.type == SCALAR)
	    add_graph_edge (graph, lhsvar, FIRST_REF_NODE + rhsvar);
	}
      else if (rhs.type == ADDRESSOF)
	{
	  /* x = &y */
	  gcc_checking_assert (find (rhs.var) == rhs.var);
	  bitmap_set_bit (get_varinfo (lhsvar)->solution, rhsvar);
	}
      else if (lhsvar > anything_id
	       && lhsvar != rhsvar && lhs.offset == 0 && rhs.offset == 0)
	{
	  add_graph_edge (graph, lhsvar, rhsvar);
	}
    }

  /* Add edges from STOREDANYTHING to all non-direct nodes that can
     receive pointers.  */
  t = find (storedanything_id);
  for (i = integer_id + 1; i < FIRST_REF_NODE; ++i)
    {
      if (!bitmap_bit_p (graph->direct_nodes, i)
	  && get_varinfo (i)->may_have_pointers)
	add_graph_edge (graph, find (i), t);
    }

  /* Everything stored to ANYTHING also potentially escapes.  */
  add_graph_edge (graph, find (escaped_id), t);
}


/* Changed variables on the last iteration.  */
static bitmap changed;

/* Strongly Connected Component visitation info.  */

struct scc_info
{
  sbitmap visited;
  sbitmap deleted;
  unsigned int *dfs;
  unsigned int *node_mapping;
  int current_index;
  vec<unsigned> scc_stack;
};


/* Recursive routine to find strongly connected components in GRAPH.
   SI is the SCC info to store the information in, and N is the id of current
   graph node we are processing.

   This is Tarjan's strongly connected component finding algorithm, as
   modified by Nuutila to keep only non-root nodes on the stack.
   The algorithm can be found in "On finding the strongly connected
   connected components in a directed graph" by Esko Nuutila and Eljas
   Soisalon-Soininen, in Information Processing Letters volume 49,
   number 1, pages 9-14.  */

static void
scc_visit (constraint_graph_t graph, struct scc_info *si, unsigned int n)
{
  unsigned int i;
  bitmap_iterator bi;
  unsigned int my_dfs;

  bitmap_set_bit (si->visited, n);
  si->dfs[n] = si->current_index ++;
  my_dfs = si->dfs[n];

  /* Visit all the successors.  */
  EXECUTE_IF_IN_NONNULL_BITMAP (graph->succs[n], 0, i, bi)
    {
      unsigned int w;

      if (i > LAST_REF_NODE)
	break;

      w = find (i);
      if (bitmap_bit_p (si->deleted, w))
	continue;

      if (!bitmap_bit_p (si->visited, w))
	scc_visit (graph, si, w);

      unsigned int t = find (w);
      gcc_checking_assert (find (n) == n);
      if (si->dfs[t] < si->dfs[n])
	si->dfs[n] = si->dfs[t];
    }

  /* See if any components have been identified.  */
  if (si->dfs[n] == my_dfs)
    {
      if (si->scc_stack.length () > 0
	  && si->dfs[si->scc_stack.last ()] >= my_dfs)
	{
	  bitmap scc = BITMAP_ALLOC (NULL);
	  unsigned int lowest_node;
	  bitmap_iterator bi;

	  bitmap_set_bit (scc, n);

	  while (si->scc_stack.length () != 0
		 && si->dfs[si->scc_stack.last ()] >= my_dfs)
	    {
	      unsigned int w = si->scc_stack.pop ();

	      bitmap_set_bit (scc, w);
	    }

	  lowest_node = bitmap_first_set_bit (scc);
	  gcc_assert (lowest_node < FIRST_REF_NODE);

	  /* Collapse the SCC nodes into a single node, and mark the
	     indirect cycles.  */
	  EXECUTE_IF_SET_IN_BITMAP (scc, 0, i, bi)
	    {
	      if (i < FIRST_REF_NODE)
		{
		  if (unite (lowest_node, i))
		    unify_nodes (graph, lowest_node, i, false);
		}
	      else
		{
		  unite (lowest_node, i);
		  graph->indirect_cycles[i - FIRST_REF_NODE] = lowest_node;
		}
	    }
	}
      bitmap_set_bit (si->deleted, n);
    }
  else
    si->scc_stack.safe_push (n);
}

/* Unify node FROM into node TO, updating the changed count if
   necessary when UPDATE_CHANGED is true.  */

static void
unify_nodes (constraint_graph_t graph, unsigned int to, unsigned int from,
	     bool update_changed)
{
  gcc_checking_assert (to != from && find (to) == to);

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "Unifying %s to %s\n",
	     get_varinfo (from)->name,
	     get_varinfo (to)->name);

  if (update_changed)
    stats.unified_vars_dynamic++;
  else
    stats.unified_vars_static++;

  merge_graph_nodes (graph, to, from);
  merge_node_constraints (graph, to, from);

  /* Mark TO as changed if FROM was changed. If TO was already marked
     as changed, decrease the changed count.  */

  if (update_changed
      && bitmap_clear_bit (changed, from))
    bitmap_set_bit (changed, to);
  varinfo_t fromvi = get_varinfo (from);
  if (fromvi->solution)
    {
      /* If the solution changes because of the merging, we need to mark
	 the variable as changed.  */
      varinfo_t tovi = get_varinfo (to);
      if (bitmap_ior_into (tovi->solution, fromvi->solution))
	{
	  if (update_changed)
	    bitmap_set_bit (changed, to);
	}

      BITMAP_FREE (fromvi->solution);
      if (fromvi->oldsolution)
	BITMAP_FREE (fromvi->oldsolution);

      if (stats.iterations > 0
	  && tovi->oldsolution)
	BITMAP_FREE (tovi->oldsolution);
    }
  if (graph->succs[to])
    bitmap_clear_bit (graph->succs[to], to);
}

/* Information needed to compute the topological ordering of a graph.  */

struct topo_info
{
  /* sbitmap of visited nodes.  */
  sbitmap visited;
  /* Array that stores the topological order of the graph, *in
     reverse*.  */
  vec<unsigned> topo_order;
};


/* Initialize and return a topological info structure.  */

static struct topo_info *
init_topo_info (void)
{
  size_t size = graph->size;
  struct topo_info *ti = XNEW (struct topo_info);
  ti->visited = sbitmap_alloc (size);
  bitmap_clear (ti->visited);
  ti->topo_order.create (1);
  return ti;
}


/* Free the topological sort info pointed to by TI.  */

static void
free_topo_info (struct topo_info *ti)
{
  sbitmap_free (ti->visited);
  ti->topo_order.release ();
  free (ti);
}

/* Visit the graph in topological order, and store the order in the
   topo_info structure.  */

static void
topo_visit (constraint_graph_t graph, struct topo_info *ti,
	    unsigned int n)
{
  bitmap_iterator bi;
  unsigned int j;

  bitmap_set_bit (ti->visited, n);

  if (graph->succs[n])
    EXECUTE_IF_SET_IN_BITMAP (graph->succs[n], 0, j, bi)
      {
	if (!bitmap_bit_p (ti->visited, j))
	  topo_visit (graph, ti, j);
      }

  ti->topo_order.safe_push (n);
}

/* Process a constraint C that represents x = *(y + off), using DELTA as the
   starting solution for y.  */

static void
do_sd_constraint (constraint_graph_t graph, constraint_t c,
		  bitmap delta)
{
  unsigned int lhs = c->lhs.var;
  bool flag = false;
  bitmap sol = get_varinfo (lhs)->solution;
  unsigned int j;
  bitmap_iterator bi;
  HOST_WIDE_INT roffset = c->rhs.offset;

  /* Our IL does not allow this.  */
  gcc_checking_assert (c->lhs.offset == 0);

  /* If the solution of Y contains anything it is good enough to transfer
     this to the LHS.  */
  if (bitmap_bit_p (delta, anything_id))
    {
      flag |= bitmap_set_bit (sol, anything_id);
      goto done;
    }

  /* If we do not know at with offset the rhs is dereferenced compute
     the reachability set of DELTA, conservatively assuming it is
     dereferenced at all valid offsets.  */
  if (roffset == UNKNOWN_OFFSET)
    {
      solution_set_expand (delta);
      /* No further offset processing is necessary.  */
      roffset = 0;
    }

  /* For each variable j in delta (Sol(y)), add
     an edge in the graph from j to x, and union Sol(j) into Sol(x).  */
  EXECUTE_IF_SET_IN_BITMAP (delta, 0, j, bi)
    {
      varinfo_t v = get_varinfo (j);
      HOST_WIDE_INT fieldoffset = v->offset + roffset;
      unsigned int t;

      if (v->is_full_var)
	fieldoffset = v->offset;
      else if (roffset != 0)
	v = first_vi_for_offset (v, fieldoffset);
      /* If the access is outside of the variable we can ignore it.  */
      if (!v)
	continue;

      do
	{
	  t = find (v->id);

	  /* Adding edges from the special vars is pointless.
	     They don't have sets that can change.  */
	  if (get_varinfo (t)->is_special_var)
	    flag |= bitmap_ior_into (sol, get_varinfo (t)->solution);
	  /* Merging the solution from ESCAPED needlessly increases
	     the set.  Use ESCAPED as representative instead.  */
	  else if (v->id == escaped_id)
	    flag |= bitmap_set_bit (sol, escaped_id);
	  else if (v->may_have_pointers
		   && add_graph_edge (graph, lhs, t))
	    flag |= bitmap_ior_into (sol, get_varinfo (t)->solution);

	  /* If the variable is not exactly at the requested offset
	     we have to include the next one.  */
	  if (v->offset == (unsigned HOST_WIDE_INT)fieldoffset
	      || v->next == 0)
	    break;

	  v = vi_next (v);
	  fieldoffset = v->offset;
	}
      while (1);
    }

done:
  /* If the LHS solution changed, mark the var as changed.  */
  if (flag)
    {
      get_varinfo (lhs)->solution = sol;
      bitmap_set_bit (changed, lhs);
    }
}

/* Process a constraint C that represents *(x + off) = y using DELTA
   as the starting solution for x.  */

static void
do_ds_constraint (constraint_t c, bitmap delta)
{
  unsigned int rhs = c->rhs.var;
  bitmap sol = get_varinfo (rhs)->solution;
  unsigned int j;
  bitmap_iterator bi;
  HOST_WIDE_INT loff = c->lhs.offset;
  bool escaped_p = false;

  /* Our IL does not allow this.  */
  gcc_checking_assert (c->rhs.offset == 0);

  /* If the solution of y contains ANYTHING simply use the ANYTHING
     solution.  This avoids needlessly increasing the points-to sets.  */
  if (bitmap_bit_p (sol, anything_id))
    sol = get_varinfo (find (anything_id))->solution;

  /* If the solution for x contains ANYTHING we have to merge the
     solution of y into all pointer variables which we do via
     STOREDANYTHING.  */
  if (bitmap_bit_p (delta, anything_id))
    {
      unsigned t = find (storedanything_id);
      if (add_graph_edge (graph, t, rhs))
	{
	  if (bitmap_ior_into (get_varinfo (t)->solution, sol))
	    bitmap_set_bit (changed, t);
	}
      return;
    }

  /* If we do not know at with offset the rhs is dereferenced compute
     the reachability set of DELTA, conservatively assuming it is
     dereferenced at all valid offsets.  */
  if (loff == UNKNOWN_OFFSET)
    {
      solution_set_expand (delta);
      loff = 0;
    }

  /* For each member j of delta (Sol(x)), add an edge from y to j and
     union Sol(y) into Sol(j) */
  EXECUTE_IF_SET_IN_BITMAP (delta, 0, j, bi)
    {
      varinfo_t v = get_varinfo (j);
      unsigned int t;
      HOST_WIDE_INT fieldoffset = v->offset + loff;

      if (v->is_full_var)
	fieldoffset = v->offset;
      else if (loff != 0)
	v = first_vi_for_offset (v, fieldoffset);
      /* If the access is outside of the variable we can ignore it.  */
      if (!v)
	continue;

      do
	{
	  if (v->may_have_pointers)
	    {
	      /* If v is a global variable then this is an escape point.  */
	      if (v->is_global_var
		  && !escaped_p)
		{
		  t = find (escaped_id);
		  if (add_graph_edge (graph, t, rhs)
		      && bitmap_ior_into (get_varinfo (t)->solution, sol))
		    bitmap_set_bit (changed, t);
		  /* Enough to let rhs escape once.  */
		  escaped_p = true;
		}

	      if (v->is_special_var)
		break;

	      t = find (v->id);
	      if (add_graph_edge (graph, t, rhs)
		  && bitmap_ior_into (get_varinfo (t)->solution, sol))
		bitmap_set_bit (changed, t);
	    }

	  /* If the variable is not exactly at the requested offset
	     we have to include the next one.  */
	  if (v->offset == (unsigned HOST_WIDE_INT)fieldoffset
	      || v->next == 0)
	    break;

	  v = vi_next (v);
	  fieldoffset = v->offset;
	}
      while (1);
    }
}

/* Handle a non-simple (simple meaning requires no iteration),
   constraint (IE *x = &y, x = *y, *x = y, and x = y with offsets involved).  */

static void
do_complex_constraint (constraint_graph_t graph, constraint_t c, bitmap delta)
{
  if (c->lhs.type == DEREF)
    {
      if (c->rhs.type == ADDRESSOF)
	{
	  gcc_unreachable();
	}
      else
	{
	  /* *x = y */
	  do_ds_constraint (c, delta);
	}
    }
  else if (c->rhs.type == DEREF)
    {
      /* x = *y */
      if (!(get_varinfo (c->lhs.var)->is_special_var))
	do_sd_constraint (graph, c, delta);
    }
  else
    {
      bitmap tmp;
      bitmap solution;
      bool flag = false;

      gcc_checking_assert (c->rhs.type == SCALAR && c->lhs.type == SCALAR);
      solution = get_varinfo (c->rhs.var)->solution;
      tmp = get_varinfo (c->lhs.var)->solution;

      flag = set_union_with_increment (tmp, solution, c->rhs.offset);

      if (flag)
	bitmap_set_bit (changed, c->lhs.var);
    }
}

/* Initialize and return a new SCC info structure.  */

static struct scc_info *
init_scc_info (size_t size)
{
  struct scc_info *si = XNEW (struct scc_info);
  size_t i;

  si->current_index = 0;
  si->visited = sbitmap_alloc (size);
  bitmap_clear (si->visited);
  si->deleted = sbitmap_alloc (size);
  bitmap_clear (si->deleted);
  si->node_mapping = XNEWVEC (unsigned int, size);
  si->dfs = XCNEWVEC (unsigned int, size);

  for (i = 0; i < size; i++)
    si->node_mapping[i] = i;

  si->scc_stack.create (1);
  return si;
}

/* Free an SCC info structure pointed to by SI */

static void
free_scc_info (struct scc_info *si)
{
  sbitmap_free (si->visited);
  sbitmap_free (si->deleted);
  free (si->node_mapping);
  free (si->dfs);
  si->scc_stack.release ();
  free (si);
}


/* Find indirect cycles in GRAPH that occur, using strongly connected
   components, and note them in the indirect cycles map.

   This technique comes from Ben Hardekopf and Calvin Lin,
   "It Pays to be Lazy: Fast and Accurate Pointer Analysis for Millions of
   Lines of Code", submitted to PLDI 2007.  */

static void
find_indirect_cycles (constraint_graph_t graph)
{
  unsigned int i;
  unsigned int size = graph->size;
  struct scc_info *si = init_scc_info (size);

  for (i = 0; i < MIN (LAST_REF_NODE, size); i ++ )
    if (!bitmap_bit_p (si->visited, i) && find (i) == i)
      scc_visit (graph, si, i);

  free_scc_info (si);
}

/* Compute a topological ordering for GRAPH, and store the result in the
   topo_info structure TI.  */

static void
compute_topo_order (constraint_graph_t graph,
		    struct topo_info *ti)
{
  unsigned int i;
  unsigned int size = graph->size;

  for (i = 0; i != size; ++i)
    if (!bitmap_bit_p (ti->visited, i) && find (i) == i)
      topo_visit (graph, ti, i);
}

/* Structure used to for hash value numbering of pointer equivalence
   classes.  */

typedef struct equiv_class_label
{
  hashval_t hashcode;
  unsigned int equivalence_class;
  bitmap labels;
} *equiv_class_label_t;
typedef const struct equiv_class_label *const_equiv_class_label_t;

/* Equiv_class_label hashtable helpers.  */

struct equiv_class_hasher : typed_free_remove <equiv_class_label>
{
  typedef equiv_class_label value_type;
  typedef equiv_class_label compare_type;
  static inline hashval_t hash (const value_type *);
  static inline bool equal (const value_type *, const compare_type *);
};

/* Hash function for a equiv_class_label_t */

inline hashval_t
equiv_class_hasher::hash (const value_type *ecl)
{
  return ecl->hashcode;
}

/* Equality function for two equiv_class_label_t's.  */

inline bool
equiv_class_hasher::equal (const value_type *eql1, const compare_type *eql2)
{
  return (eql1->hashcode == eql2->hashcode
	  && bitmap_equal_p (eql1->labels, eql2->labels));
}

/* A hashtable for mapping a bitmap of labels->pointer equivalence
   classes.  */
static hash_table <equiv_class_hasher> pointer_equiv_class_table;

/* A hashtable for mapping a bitmap of labels->location equivalence
   classes.  */
static hash_table <equiv_class_hasher> location_equiv_class_table;

/* Lookup a equivalence class in TABLE by the bitmap of LABELS with
   hash HAS it contains.  Sets *REF_LABELS to the bitmap LABELS
   is equivalent to.  */

static equiv_class_label *
equiv_class_lookup_or_add (hash_table <equiv_class_hasher> table, bitmap labels)
{
  equiv_class_label **slot;
  equiv_class_label ecl;

  ecl.labels = labels;
  ecl.hashcode = bitmap_hash (labels);
  slot = table.find_slot_with_hash (&ecl, ecl.hashcode, INSERT);
  if (!*slot)
    {
      *slot = XNEW (struct equiv_class_label);
      (*slot)->labels = labels;
      (*slot)->hashcode = ecl.hashcode;
      (*slot)->equivalence_class = 0;
    }

  return *slot;
}

/* Perform offline variable substitution.

   This is a worst case quadratic time way of identifying variables
   that must have equivalent points-to sets, including those caused by
   static cycles, and single entry subgraphs, in the constraint graph.

   The technique is described in "Exploiting Pointer and Location
   Equivalence to Optimize Pointer Analysis. In the 14th International
   Static Analysis Symposium (SAS), August 2007."  It is known as the
   "HU" algorithm, and is equivalent to value numbering the collapsed
   constraint graph including evaluating unions.

   The general method of finding equivalence classes is as follows:
   Add fake nodes (REF nodes) and edges for *a = b and a = *b constraints.
   Initialize all non-REF nodes to be direct nodes.
   For each constraint a = a U {b}, we set pts(a) = pts(a) u {fresh
   variable}
   For each constraint containing the dereference, we also do the same
   thing.

   We then compute SCC's in the graph and unify nodes in the same SCC,
   including pts sets.

   For each non-collapsed node x:
    Visit all unvisited explicit incoming edges.
    Ignoring all non-pointers, set pts(x) = Union of pts(a) for y
    where y->x.
    Lookup the equivalence class for pts(x).
     If we found one, equivalence_class(x) = found class.
     Otherwise, equivalence_class(x) = new class, and new_class is
    added to the lookup table.

   All direct nodes with the same equivalence class can be replaced
   with a single representative node.
   All unlabeled nodes (label == 0) are not pointers and all edges
   involving them can be eliminated.
   We perform these optimizations during rewrite_constraints

   In addition to pointer equivalence class finding, we also perform
   location equivalence class finding.  This is the set of variables
   that always appear together in points-to sets.  We use this to
   compress the size of the points-to sets.  */

/* Current maximum pointer equivalence class id.  */
static int pointer_equiv_class;

/* Current maximum location equivalence class id.  */
static int location_equiv_class;

/* Recursive routine to find strongly connected components in GRAPH,
   and label it's nodes with DFS numbers.  */

static void
condense_visit (constraint_graph_t graph, struct scc_info *si, unsigned int n)
{
  unsigned int i;
  bitmap_iterator bi;
  unsigned int my_dfs;

  gcc_checking_assert (si->node_mapping[n] == n);
  bitmap_set_bit (si->visited, n);
  si->dfs[n] = si->current_index ++;
  my_dfs = si->dfs[n];

  /* Visit all the successors.  */
  EXECUTE_IF_IN_NONNULL_BITMAP (graph->preds[n], 0, i, bi)
    {
      unsigned int w = si->node_mapping[i];

      if (bitmap_bit_p (si->deleted, w))
	continue;

      if (!bitmap_bit_p (si->visited, w))
	condense_visit (graph, si, w);

      unsigned int t = si->node_mapping[w];
      gcc_checking_assert (si->node_mapping[n] == n);
      if (si->dfs[t] < si->dfs[n])
	si->dfs[n] = si->dfs[t];
    }

  /* Visit all the implicit predecessors.  */
  EXECUTE_IF_IN_NONNULL_BITMAP (graph->implicit_preds[n], 0, i, bi)
    {
      unsigned int w = si->node_mapping[i];

      if (bitmap_bit_p (si->deleted, w))
	continue;

      if (!bitmap_bit_p (si->visited, w))
	condense_visit (graph, si, w);

      unsigned int t = si->node_mapping[w];
      gcc_assert (si->node_mapping[n] == n);
      if (si->dfs[t] < si->dfs[n])
	si->dfs[n] = si->dfs[t];
    }

  /* See if any components have been identified.  */
  if (si->dfs[n] == my_dfs)
    {
      while (si->scc_stack.length () != 0
	     && si->dfs[si->scc_stack.last ()] >= my_dfs)
	{
	  unsigned int w = si->scc_stack.pop ();
	  si->node_mapping[w] = n;

	  if (!bitmap_bit_p (graph->direct_nodes, w))
	    bitmap_clear_bit (graph->direct_nodes, n);

	  /* Unify our nodes.  */
	  if (graph->preds[w])
	    {
	      if (!graph->preds[n])
		graph->preds[n] = BITMAP_ALLOC (&predbitmap_obstack);
	      bitmap_ior_into (graph->preds[n], graph->preds[w]);
	    }
	  if (graph->implicit_preds[w])
	    {
	      if (!graph->implicit_preds[n])
		graph->implicit_preds[n] = BITMAP_ALLOC (&predbitmap_obstack);
	      bitmap_ior_into (graph->implicit_preds[n],
			       graph->implicit_preds[w]);
	    }
	  if (graph->points_to[w])
	    {
	      if (!graph->points_to[n])
		graph->points_to[n] = BITMAP_ALLOC (&predbitmap_obstack);
	      bitmap_ior_into (graph->points_to[n],
			       graph->points_to[w]);
	    }
	}
      bitmap_set_bit (si->deleted, n);
    }
  else
    si->scc_stack.safe_push (n);
}

/* Label pointer equivalences.  */

static void
label_visit (constraint_graph_t graph, struct scc_info *si, unsigned int n)
{
  unsigned int i, first_pred;
  bitmap_iterator bi;

  bitmap_set_bit (si->visited, n);

  /* Label and union our incoming edges's points to sets.  */
  first_pred = -1U;
  EXECUTE_IF_IN_NONNULL_BITMAP (graph->preds[n], 0, i, bi)
    {
      unsigned int w = si->node_mapping[i];
      if (!bitmap_bit_p (si->visited, w))
	label_visit (graph, si, w);

      /* Skip unused edges  */
      if (w == n || graph->pointer_label[w] == 0)
	continue;

      if (graph->points_to[w])
	{
	  if (!graph->points_to[n])
	    {
	      if (first_pred == -1U)
		first_pred = w;
	      else
		{
		  graph->points_to[n] = BITMAP_ALLOC (&predbitmap_obstack);
		  bitmap_ior (graph->points_to[n],
			      graph->points_to[first_pred],
			      graph->points_to[w]);
		}
	    }
	  else
	    bitmap_ior_into(graph->points_to[n], graph->points_to[w]);
	}
    }

  /* Indirect nodes get fresh variables and a new pointer equiv class.  */
  if (!bitmap_bit_p (graph->direct_nodes, n))
    {
      if (!graph->points_to[n])
	{
	  graph->points_to[n] = BITMAP_ALLOC (&predbitmap_obstack);
	  if (first_pred != -1U)
	    bitmap_copy (graph->points_to[n], graph->points_to[first_pred]);
	}
      bitmap_set_bit (graph->points_to[n], FIRST_REF_NODE + n);
      graph->pointer_label[n] = pointer_equiv_class++;
      equiv_class_label_t ecl;
      ecl = equiv_class_lookup_or_add (pointer_equiv_class_table,
				       graph->points_to[n]);
      ecl->equivalence_class = graph->pointer_label[n];
      return;
    }

  /* If there was only a single non-empty predecessor the pointer equiv
     class is the same.  */
  if (!graph->points_to[n])
    {
      if (first_pred != -1U)
	{
	  graph->pointer_label[n] = graph->pointer_label[first_pred];
	  graph->points_to[n] = graph->points_to[first_pred];
	}
      return;
    }

  if (!bitmap_empty_p (graph->points_to[n]))
    {
      equiv_class_label_t ecl;
      ecl = equiv_class_lookup_or_add (pointer_equiv_class_table,
				       graph->points_to[n]);
      if (ecl->equivalence_class == 0)
	ecl->equivalence_class = pointer_equiv_class++;
      else
	{
	  BITMAP_FREE (graph->points_to[n]);
	  graph->points_to[n] = ecl->labels;
	}
      graph->pointer_label[n] = ecl->equivalence_class;
    }
}

/* Print the pred graph in dot format.  */

static void
dump_pred_graph (struct scc_info *si, FILE *file)
{
  unsigned int i;

  /* Only print the graph if it has already been initialized:  */
  if (!graph)
    return;

  /* Prints the header of the dot file:  */
  fprintf (file, "strict digraph {\n");
  fprintf (file, "  node [\n    shape = box\n  ]\n");
  fprintf (file, "  edge [\n    fontsize = \"12\"\n  ]\n");
  fprintf (file, "\n  // List of nodes and complex constraints in "
	   "the constraint graph:\n");

  /* The next lines print the nodes in the graph together with the
     complex constraints attached to them.  */
  for (i = 1; i < graph->size; i++)
    {
      if (i == FIRST_REF_NODE)
	continue;
      if (si->node_mapping[i] != i)
	continue;
      if (i < FIRST_REF_NODE)
	fprintf (file, "\"%s\"", get_varinfo (i)->name);
      else
	fprintf (file, "\"*%s\"", get_varinfo (i - FIRST_REF_NODE)->name);
      if (graph->points_to[i]
	  && !bitmap_empty_p (graph->points_to[i]))
	{
	  fprintf (file, "[label=\"%s = {", get_varinfo (i)->name);
	  unsigned j;
	  bitmap_iterator bi;
	  EXECUTE_IF_SET_IN_BITMAP (graph->points_to[i], 0, j, bi)
	    fprintf (file, " %d", j);
	  fprintf (file, " }\"]");
	}
      fprintf (file, ";\n");
    }

  /* Go over the edges.  */
  fprintf (file, "\n  // Edges in the constraint graph:\n");
  for (i = 1; i < graph->size; i++)
    {
      unsigned j;
      bitmap_iterator bi;
      if (si->node_mapping[i] != i)
	continue;
      EXECUTE_IF_IN_NONNULL_BITMAP (graph->preds[i], 0, j, bi)
	{
	  unsigned from = si->node_mapping[j];
	  if (from < FIRST_REF_NODE)
	    fprintf (file, "\"%s\"", get_varinfo (from)->name);
	  else
	    fprintf (file, "\"*%s\"", get_varinfo (from - FIRST_REF_NODE)->name);
	  fprintf (file, " -> ");
	  if (i < FIRST_REF_NODE)
	    fprintf (file, "\"%s\"", get_varinfo (i)->name);
	  else
	    fprintf (file, "\"*%s\"", get_varinfo (i - FIRST_REF_NODE)->name);
	  fprintf (file, ";\n");
	}
    }

  /* Prints the tail of the dot file.  */
  fprintf (file, "}\n");
}

/* Perform offline variable substitution, discovering equivalence
   classes, and eliminating non-pointer variables.  */

static struct scc_info *
perform_var_substitution (constraint_graph_t graph)
{
  unsigned int i;
  unsigned int size = graph->size;
  struct scc_info *si = init_scc_info (size);

  bitmap_obstack_initialize (&iteration_obstack);
  pointer_equiv_class_table.create (511);
  location_equiv_class_table.create (511);
  pointer_equiv_class = 1;
  location_equiv_class = 1;

  /* Condense the nodes, which means to find SCC's, count incoming
     predecessors, and unite nodes in SCC's.  */
  for (i = 1; i < FIRST_REF_NODE; i++)
    if (!bitmap_bit_p (si->visited, si->node_mapping[i]))
      condense_visit (graph, si, si->node_mapping[i]);

  if (dump_file && (dump_flags & TDF_GRAPH))
    {
      fprintf (dump_file, "\n\n// The constraint graph before var-substitution "
	       "in dot format:\n");
      dump_pred_graph (si, dump_file);
      fprintf (dump_file, "\n\n");
    }

  bitmap_clear (si->visited);
  /* Actually the label the nodes for pointer equivalences  */
  for (i = 1; i < FIRST_REF_NODE; i++)
    if (!bitmap_bit_p (si->visited, si->node_mapping[i]))
      label_visit (graph, si, si->node_mapping[i]);

  /* Calculate location equivalence labels.  */
  for (i = 1; i < FIRST_REF_NODE; i++)
    {
      bitmap pointed_by;
      bitmap_iterator bi;
      unsigned int j;

      if (!graph->pointed_by[i])
	continue;
      pointed_by = BITMAP_ALLOC (&iteration_obstack);

      /* Translate the pointed-by mapping for pointer equivalence
	 labels.  */
      EXECUTE_IF_SET_IN_BITMAP (graph->pointed_by[i], 0, j, bi)
	{
	  bitmap_set_bit (pointed_by,
			  graph->pointer_label[si->node_mapping[j]]);
	}
      /* The original pointed_by is now dead.  */
      BITMAP_FREE (graph->pointed_by[i]);

      /* Look up the location equivalence label if one exists, or make
	 one otherwise.  */
      equiv_class_label_t ecl;
      ecl = equiv_class_lookup_or_add (location_equiv_class_table, pointed_by);
      if (ecl->equivalence_class == 0)
	ecl->equivalence_class = location_equiv_class++;
      else
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Found location equivalence for node %s\n",
		     get_varinfo (i)->name);
	  BITMAP_FREE (pointed_by);
	}
      graph->loc_label[i] = ecl->equivalence_class;

    }

  if (dump_file && (dump_flags & TDF_DETAILS))
    for (i = 1; i < FIRST_REF_NODE; i++)
      {
	unsigned j = si->node_mapping[i];
	if (j != i)
	  {
	    fprintf (dump_file, "%s node id %d ",
		     bitmap_bit_p (graph->direct_nodes, i)
		     ? "Direct" : "Indirect", i);
	    if (i < FIRST_REF_NODE)
	      fprintf (dump_file, "\"%s\"", get_varinfo (i)->name);
	    else
	      fprintf (dump_file, "\"*%s\"",
		       get_varinfo (i - FIRST_REF_NODE)->name);
	    fprintf (dump_file, " mapped to SCC leader node id %d ", j);
	    if (j < FIRST_REF_NODE)
	      fprintf (dump_file, "\"%s\"\n", get_varinfo (j)->name);
	    else
	      fprintf (dump_file, "\"*%s\"\n",
		       get_varinfo (j - FIRST_REF_NODE)->name);
	  }
	else
	  {
	    fprintf (dump_file,
		     "Equivalence classes for %s node id %d ",
		     bitmap_bit_p (graph->direct_nodes, i)
		     ? "direct" : "indirect", i);
	    if (i < FIRST_REF_NODE)
	      fprintf (dump_file, "\"%s\"", get_varinfo (i)->name);
	    else
	      fprintf (dump_file, "\"*%s\"",
		       get_varinfo (i - FIRST_REF_NODE)->name);
	    fprintf (dump_file,
		     ": pointer %d, location %d\n",
		     graph->pointer_label[i], graph->loc_label[i]);
	  }
      }

  /* Quickly eliminate our non-pointer variables.  */

  for (i = 1; i < FIRST_REF_NODE; i++)
    {
      unsigned int node = si->node_mapping[i];

      if (graph->pointer_label[node] == 0)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file,
		     "%s is a non-pointer variable, eliminating edges.\n",
		     get_varinfo (node)->name);
	  stats.nonpointer_vars++;
	  clear_edges_for_node (graph, node);
	}
    }

  return si;
}

/* Free information that was only necessary for variable
   substitution.  */

static void
free_var_substitution_info (struct scc_info *si)
{
  free_scc_info (si);
  free (graph->pointer_label);
  free (graph->loc_label);
  free (graph->pointed_by);
  free (graph->points_to);
  free (graph->eq_rep);
  sbitmap_free (graph->direct_nodes);
  pointer_equiv_class_table.dispose ();
  location_equiv_class_table.dispose ();
  bitmap_obstack_release (&iteration_obstack);
}

/* Return an existing node that is equivalent to NODE, which has
   equivalence class LABEL, if one exists.  Return NODE otherwise.  */

static unsigned int
find_equivalent_node (constraint_graph_t graph,
		      unsigned int node, unsigned int label)
{
  /* If the address version of this variable is unused, we can
     substitute it for anything else with the same label.
     Otherwise, we know the pointers are equivalent, but not the
     locations, and we can unite them later.  */

  if (!bitmap_bit_p (graph->address_taken, node))
    {
      gcc_checking_assert (label < graph->size);

      if (graph->eq_rep[label] != -1)
	{
	  /* Unify the two variables since we know they are equivalent.  */
	  if (unite (graph->eq_rep[label], node))
	    unify_nodes (graph, graph->eq_rep[label], node, false);
	  return graph->eq_rep[label];
	}
      else
	{
	  graph->eq_rep[label] = node;
	  graph->pe_rep[label] = node;
	}
    }
  else
    {
      gcc_checking_assert (label < graph->size);
      graph->pe[node] = label;
      if (graph->pe_rep[label] == -1)
	graph->pe_rep[label] = node;
    }

  return node;
}

/* Unite pointer equivalent but not location equivalent nodes in
   GRAPH.  This may only be performed once variable substitution is
   finished.  */

static void
unite_pointer_equivalences (constraint_graph_t graph)
{
  unsigned int i;

  /* Go through the pointer equivalences and unite them to their
     representative, if they aren't already.  */
  for (i = 1; i < FIRST_REF_NODE; i++)
    {
      unsigned int label = graph->pe[i];
      if (label)
	{
	  int label_rep = graph->pe_rep[label];

	  if (label_rep == -1)
	    continue;

	  label_rep = find (label_rep);
	  if (label_rep >= 0 && unite (label_rep, find (i)))
	    unify_nodes (graph, label_rep, i, false);
	}
    }
}

/* Move complex constraints to the GRAPH nodes they belong to.  */

static void
move_complex_constraints (constraint_graph_t graph)
{
  int i;
  constraint_t c;

  FOR_EACH_VEC_ELT (constraints, i, c)
    {
      if (c)
	{
	  struct constraint_expr lhs = c->lhs;
	  struct constraint_expr rhs = c->rhs;

	  if (lhs.type == DEREF)
	    {
	      insert_into_complex (graph, lhs.var, c);
	    }
	  else if (rhs.type == DEREF)
	    {
	      if (!(get_varinfo (lhs.var)->is_special_var))
		insert_into_complex (graph, rhs.var, c);
	    }
	  else if (rhs.type != ADDRESSOF && lhs.var > anything_id
		   && (lhs.offset != 0 || rhs.offset != 0))
	    {
	      insert_into_complex (graph, rhs.var, c);
	    }
	}
    }
}


/* Optimize and rewrite complex constraints while performing
   collapsing of equivalent nodes.  SI is the SCC_INFO that is the
   result of perform_variable_substitution.  */

static void
rewrite_constraints (constraint_graph_t graph,
		     struct scc_info *si)
{
  int i;
  constraint_t c;

#ifdef ENABLE_CHECKING
  for (unsigned int j = 0; j < graph->size; j++)
    gcc_assert (find (j) == j);
#endif

  FOR_EACH_VEC_ELT (constraints, i, c)
    {
      struct constraint_expr lhs = c->lhs;
      struct constraint_expr rhs = c->rhs;
      unsigned int lhsvar = find (lhs.var);
      unsigned int rhsvar = find (rhs.var);
      unsigned int lhsnode, rhsnode;
      unsigned int lhslabel, rhslabel;

      lhsnode = si->node_mapping[lhsvar];
      rhsnode = si->node_mapping[rhsvar];
      lhslabel = graph->pointer_label[lhsnode];
      rhslabel = graph->pointer_label[rhsnode];

      /* See if it is really a non-pointer variable, and if so, ignore
	 the constraint.  */
      if (lhslabel == 0)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {

	      fprintf (dump_file, "%s is a non-pointer variable,"
		       "ignoring constraint:",
		       get_varinfo (lhs.var)->name);
	      dump_constraint (dump_file, c);
	      fprintf (dump_file, "\n");
	    }
	  constraints[i] = NULL;
	  continue;
	}

      if (rhslabel == 0)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {

	      fprintf (dump_file, "%s is a non-pointer variable,"
		       "ignoring constraint:",
		       get_varinfo (rhs.var)->name);
	      dump_constraint (dump_file, c);
	      fprintf (dump_file, "\n");
	    }
	  constraints[i] = NULL;
	  continue;
	}

      lhsvar = find_equivalent_node (graph, lhsvar, lhslabel);
      rhsvar = find_equivalent_node (graph, rhsvar, rhslabel);
      c->lhs.var = lhsvar;
      c->rhs.var = rhsvar;
    }
}

/* Eliminate indirect cycles involving NODE.  Return true if NODE was
   part of an SCC, false otherwise.  */

static bool
eliminate_indirect_cycles (unsigned int node)
{
  if (graph->indirect_cycles[node] != -1
      && !bitmap_empty_p (get_varinfo (node)->solution))
    {
      unsigned int i;
      vec<unsigned> queue = vNULL;
      int queuepos;
      unsigned int to = find (graph->indirect_cycles[node]);
      bitmap_iterator bi;

      /* We can't touch the solution set and call unify_nodes
	 at the same time, because unify_nodes is going to do
	 bitmap unions into it. */

      EXECUTE_IF_SET_IN_BITMAP (get_varinfo (node)->solution, 0, i, bi)
	{
	  if (find (i) == i && i != to)
	    {
	      if (unite (to, i))
		queue.safe_push (i);
	    }
	}

      for (queuepos = 0;
	   queue.iterate (queuepos, &i);
	   queuepos++)
	{
	  unify_nodes (graph, to, i, true);
	}
      queue.release ();
      return true;
    }
  return false;
}

/* Solve the constraint graph GRAPH using our worklist solver.
   This is based on the PW* family of solvers from the "Efficient Field
   Sensitive Pointer Analysis for C" paper.
   It works by iterating over all the graph nodes, processing the complex
   constraints and propagating the copy constraints, until everything stops
   changed.  This corresponds to steps 6-8 in the solving list given above.  */

static void
solve_graph (constraint_graph_t graph)
{
  unsigned int size = graph->size;
  unsigned int i;
  bitmap pts;

  changed = BITMAP_ALLOC (NULL);

  /* Mark all initial non-collapsed nodes as changed.  */
  for (i = 1; i < size; i++)
    {
      varinfo_t ivi = get_varinfo (i);
      if (find (i) == i && !bitmap_empty_p (ivi->solution)
	  && ((graph->succs[i] && !bitmap_empty_p (graph->succs[i]))
	      || graph->complex[i].length () > 0))
	bitmap_set_bit (changed, i);
    }

  /* Allocate a bitmap to be used to store the changed bits.  */
  pts = BITMAP_ALLOC (&pta_obstack);

  while (!bitmap_empty_p (changed))
    {
      unsigned int i;
      struct topo_info *ti = init_topo_info ();
      stats.iterations++;

      bitmap_obstack_initialize (&iteration_obstack);

      compute_topo_order (graph, ti);

      while (ti->topo_order.length () != 0)
	{

	  i = ti->topo_order.pop ();

	  /* If this variable is not a representative, skip it.  */
	  if (find (i) != i)
	    continue;

	  /* In certain indirect cycle cases, we may merge this
	     variable to another.  */
	  if (eliminate_indirect_cycles (i) && find (i) != i)
	    continue;

	  /* If the node has changed, we need to process the
	     complex constraints and outgoing edges again.  */
	  if (bitmap_clear_bit (changed, i))
	    {
	      unsigned int j;
	      constraint_t c;
	      bitmap solution;
	      vec<constraint_t> complex = graph->complex[i];
	      varinfo_t vi = get_varinfo (i);
	      bool solution_empty;

	      /* Compute the changed set of solution bits.  If anything
	         is in the solution just propagate that.  */
	      if (bitmap_bit_p (vi->solution, anything_id))
		{
		  /* If anything is also in the old solution there is
		     nothing to do.
		     ???  But we shouldn't ended up with "changed" set ...  */
		  if (vi->oldsolution
		      && bitmap_bit_p (vi->oldsolution, anything_id))
		    continue;
		  bitmap_copy (pts, get_varinfo (find (anything_id))->solution);
		}
	      else if (vi->oldsolution)
		bitmap_and_compl (pts, vi->solution, vi->oldsolution);
	      else
		bitmap_copy (pts, vi->solution);

	      if (bitmap_empty_p (pts))
		continue;

	      if (vi->oldsolution)
		bitmap_ior_into (vi->oldsolution, pts);
	      else
		{
		  vi->oldsolution = BITMAP_ALLOC (&oldpta_obstack);
		  bitmap_copy (vi->oldsolution, pts);
		}

	      solution = vi->solution;
	      solution_empty = bitmap_empty_p (solution);

	      /* Process the complex constraints */
	      FOR_EACH_VEC_ELT (complex, j, c)
		{
		  /* XXX: This is going to unsort the constraints in
		     some cases, which will occasionally add duplicate
		     constraints during unification.  This does not
		     affect correctness.  */
		  c->lhs.var = find (c->lhs.var);
		  c->rhs.var = find (c->rhs.var);

		  /* The only complex constraint that can change our
		     solution to non-empty, given an empty solution,
		     is a constraint where the lhs side is receiving
		     some set from elsewhere.  */
		  if (!solution_empty || c->lhs.type != DEREF)
		    do_complex_constraint (graph, c, pts);
		}

	      solution_empty = bitmap_empty_p (solution);

	      if (!solution_empty)
		{
		  bitmap_iterator bi;
		  unsigned eff_escaped_id = find (escaped_id);

		  /* Propagate solution to all successors.  */
		  EXECUTE_IF_IN_NONNULL_BITMAP (graph->succs[i],
						0, j, bi)
		    {
		      bitmap tmp;
		      bool flag;

		      unsigned int to = find (j);
		      tmp = get_varinfo (to)->solution;
		      flag = false;

		      /* Don't try to propagate to ourselves.  */
		      if (to == i)
			continue;

		      /* If we propagate from ESCAPED use ESCAPED as
		         placeholder.  */
		      if (i == eff_escaped_id)
			flag = bitmap_set_bit (tmp, escaped_id);
		      else
			flag = bitmap_ior_into (tmp, pts);

		      if (flag)
			bitmap_set_bit (changed, to);
		    }
		}
	    }
	}
      free_topo_info (ti);
      bitmap_obstack_release (&iteration_obstack);
    }

  BITMAP_FREE (pts);
  BITMAP_FREE (changed);
  bitmap_obstack_release (&oldpta_obstack);
}

/* Map from trees to variable infos.  */
static struct pointer_map_t *vi_for_tree;


/* Insert ID as the variable id for tree T in the vi_for_tree map.  */

static void
insert_vi_for_tree (tree t, varinfo_t vi)
{
  void **slot = pointer_map_insert (vi_for_tree, t);
  gcc_assert (vi);
  gcc_assert (*slot == NULL);
  *slot = vi;
}

/* Find the variable info for tree T in VI_FOR_TREE.  If T does not
   exist in the map, return NULL, otherwise, return the varinfo we found.  */

static varinfo_t
lookup_vi_for_tree (tree t)
{
  void **slot = pointer_map_contains (vi_for_tree, t);
  if (slot == NULL)
    return NULL;

  return (varinfo_t) *slot;
}

/* Return a printable name for DECL  */

static const char *
alias_get_name (tree decl)
{
  const char *res = NULL;
  char *temp;
  int num_printed = 0;

  if (!dump_file)
    return "NULL";

  if (TREE_CODE (decl) == SSA_NAME)
    {
      res = get_name (decl);
      if (res)
	num_printed = asprintf (&temp, "%s_%u", res, SSA_NAME_VERSION (decl));
      else
	num_printed = asprintf (&temp, "_%u", SSA_NAME_VERSION (decl));
      if (num_printed > 0)
	{
	  res = ggc_strdup (temp);
	  free (temp);
	}
    }
  else if (DECL_P (decl))
    {
      if (DECL_ASSEMBLER_NAME_SET_P (decl))
	res = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (decl));
      else
	{
	  res = get_name (decl);
	  if (!res)
	    {
	      num_printed = asprintf (&temp, "D.%u", DECL_UID (decl));
	      if (num_printed > 0)
		{
		  res = ggc_strdup (temp);
		  free (temp);
		}
	    }
	}
    }
  if (res != NULL)
    return res;

  return "NULL";
}

/* Find the variable id for tree T in the map.
   If T doesn't exist in the map, create an entry for it and return it.  */

static varinfo_t
get_vi_for_tree (tree t)
{
  void **slot = pointer_map_contains (vi_for_tree, t);
  if (slot == NULL)
    return get_varinfo (create_variable_info_for (t, alias_get_name (t)));

  return (varinfo_t) *slot;
}

/* Get a scalar constraint expression for a new temporary variable.  */

static struct constraint_expr
new_scalar_tmp_constraint_exp (const char *name)
{
  struct constraint_expr tmp;
  varinfo_t vi;

  vi = new_var_info (NULL_TREE, name);
  vi->offset = 0;
  vi->size = -1;
  vi->fullsize = -1;
  vi->is_full_var = 1;

  tmp.var = vi->id;
  tmp.type = SCALAR;
  tmp.offset = 0;

  return tmp;
}

/* Get a constraint expression vector from an SSA_VAR_P node.
   If address_p is true, the result will be taken its address of.  */

static void
get_constraint_for_ssa_var (tree t, vec<ce_s> *results, bool address_p)
{
  struct constraint_expr cexpr;
  varinfo_t vi;

  /* We allow FUNCTION_DECLs here even though it doesn't make much sense.  */
  gcc_assert (TREE_CODE (t) == SSA_NAME || DECL_P (t));

  /* For parameters, get at the points-to set for the actual parm
     decl.  */
  if (TREE_CODE (t) == SSA_NAME
      && SSA_NAME_IS_DEFAULT_DEF (t)
      && (TREE_CODE (SSA_NAME_VAR (t)) == PARM_DECL
	  || TREE_CODE (SSA_NAME_VAR (t)) == RESULT_DECL))
    {
      get_constraint_for_ssa_var (SSA_NAME_VAR (t), results, address_p);
      return;
    }

  /* For global variables resort to the alias target.  */
  if (TREE_CODE (t) == VAR_DECL
      && (TREE_STATIC (t) || DECL_EXTERNAL (t)))
    {
      struct varpool_node *node = varpool_get_node (t);
      if (node && node->symbol.alias && node->symbol.analyzed)
	{
	  node = varpool_variable_node (node, NULL);
	  t = node->symbol.decl;
	}
    }

  vi = get_vi_for_tree (t);
  cexpr.var = vi->id;
  cexpr.type = SCALAR;
  cexpr.offset = 0;
  /* If we determine the result is "anything", and we know this is readonly,
     say it points to readonly memory instead.  */
  if (cexpr.var == anything_id && TREE_READONLY (t))
    {
      gcc_unreachable ();
      cexpr.type = ADDRESSOF;
      cexpr.var = readonly_id;
    }

  /* If we are not taking the address of the constraint expr, add all
     sub-fiels of the variable as well.  */
  if (!address_p
      && !vi->is_full_var)
    {
      for (; vi; vi = vi_next (vi))
	{
	  cexpr.var = vi->id;
	  results->safe_push (cexpr);
	}
      return;
    }

  results->safe_push (cexpr);
}

/* Process constraint T, performing various simplifications and then
   adding it to our list of overall constraints.  */

static void
process_constraint (constraint_t t)
{
  struct constraint_expr rhs = t->rhs;
  struct constraint_expr lhs = t->lhs;

  gcc_assert (rhs.var < varmap.length ());
  gcc_assert (lhs.var < varmap.length ());

  /* If we didn't get any useful constraint from the lhs we get
     &ANYTHING as fallback from get_constraint_for.  Deal with
     it here by turning it into *ANYTHING.  */
  if (lhs.type == ADDRESSOF
      && lhs.var == anything_id)
    lhs.type = DEREF;

  /* ADDRESSOF on the lhs is invalid.  */
  gcc_assert (lhs.type != ADDRESSOF);

  /* We shouldn't add constraints from things that cannot have pointers.
     It's not completely trivial to avoid in the callers, so do it here.  */
  if (rhs.type != ADDRESSOF
      && !get_varinfo (rhs.var)->may_have_pointers)
    return;

  /* Likewise adding to the solution of a non-pointer var isn't useful.  */
  if (!get_varinfo (lhs.var)->may_have_pointers)
    return;

  /* This can happen in our IR with things like n->a = *p */
  if (rhs.type == DEREF && lhs.type == DEREF && rhs.var != anything_id)
    {
      /* Split into tmp = *rhs, *lhs = tmp */
      struct constraint_expr tmplhs;
      tmplhs = new_scalar_tmp_constraint_exp ("doubledereftmp");
      process_constraint (new_constraint (tmplhs, rhs));
      process_constraint (new_constraint (lhs, tmplhs));
    }
  else if (rhs.type == ADDRESSOF && lhs.type == DEREF)
    {
      /* Split into tmp = &rhs, *lhs = tmp */
      struct constraint_expr tmplhs;
      tmplhs = new_scalar_tmp_constraint_exp ("derefaddrtmp");
      process_constraint (new_constraint (tmplhs, rhs));
      process_constraint (new_constraint (lhs, tmplhs));
    }
  else
    {
      gcc_assert (rhs.type != ADDRESSOF || rhs.offset == 0);
      constraints.safe_push (t);
    }
}


/* Return the position, in bits, of FIELD_DECL from the beginning of its
   structure.  */

static HOST_WIDE_INT
bitpos_of_field (const tree fdecl)
{
  if (!host_integerp (DECL_FIELD_OFFSET (fdecl), 0)
      || !host_integerp (DECL_FIELD_BIT_OFFSET (fdecl), 0))
    return -1;

  return (TREE_INT_CST_LOW (DECL_FIELD_OFFSET (fdecl)) * BITS_PER_UNIT
	  + TREE_INT_CST_LOW (DECL_FIELD_BIT_OFFSET (fdecl)));
}


/* Get constraint expressions for offsetting PTR by OFFSET.  Stores the
   resulting constraint expressions in *RESULTS.  */

static void
get_constraint_for_ptr_offset (tree ptr, tree offset,
			       vec<ce_s> *results)
{
  struct constraint_expr c;
  unsigned int j, n;
  HOST_WIDE_INT rhsoffset;

  /* If we do not do field-sensitive PTA adding offsets to pointers
     does not change the points-to solution.  */
  if (!use_field_sensitive)
    {
      get_constraint_for_rhs (ptr, results);
      return;
    }

  /* If the offset is not a non-negative integer constant that fits
     in a HOST_WIDE_INT, we have to fall back to a conservative
     solution which includes all sub-fields of all pointed-to
     variables of ptr.  */
  if (offset == NULL_TREE
      || TREE_CODE (offset) != INTEGER_CST)
    rhsoffset = UNKNOWN_OFFSET;
  else
    {
      /* Sign-extend the offset.  */
      double_int soffset = tree_to_double_int (offset)
			   .sext (TYPE_PRECISION (TREE_TYPE (offset)));
      if (!soffset.fits_shwi ())
	rhsoffset = UNKNOWN_OFFSET;
      else
	{
	  /* Make sure the bit-offset also fits.  */
	  HOST_WIDE_INT rhsunitoffset = soffset.low;
	  rhsoffset = rhsunitoffset * BITS_PER_UNIT;
	  if (rhsunitoffset != rhsoffset / BITS_PER_UNIT)
	    rhsoffset = UNKNOWN_OFFSET;
	}
    }

  get_constraint_for_rhs (ptr, results);
  if (rhsoffset == 0)
    return;

  /* As we are eventually appending to the solution do not use
     vec::iterate here.  */
  n = results->length ();
  for (j = 0; j < n; j++)
    {
      varinfo_t curr;
      c = (*results)[j];
      curr = get_varinfo (c.var);

      if (c.type == ADDRESSOF
	  /* If this varinfo represents a full variable just use it.  */
	  && curr->is_full_var)
	c.offset = 0;
      else if (c.type == ADDRESSOF
	       /* If we do not know the offset add all subfields.  */
	       && rhsoffset == UNKNOWN_OFFSET)
	{
	  varinfo_t temp = get_varinfo (curr->head);
	  do
	    {
	      struct constraint_expr c2;
	      c2.var = temp->id;
	      c2.type = ADDRESSOF;
	      c2.offset = 0;
	      if (c2.var != c.var)
		results->safe_push (c2);
	      temp = vi_next (temp);
	    }
	  while (temp);
	}
      else if (c.type == ADDRESSOF)
	{
	  varinfo_t temp;
	  unsigned HOST_WIDE_INT offset = curr->offset + rhsoffset;

	  /* Search the sub-field which overlaps with the
	     pointed-to offset.  If the result is outside of the variable
	     we have to provide a conservative result, as the variable is
	     still reachable from the resulting pointer (even though it
	     technically cannot point to anything).  The last and first
	     sub-fields are such conservative results.
	     ???  If we always had a sub-field for &object + 1 then
	     we could represent this in a more precise way.  */
	  if (rhsoffset < 0
	      && curr->offset < offset)
	    offset = 0;
	  temp = first_or_preceding_vi_for_offset (curr, offset);

	  /* If the found variable is not exactly at the pointed to
	     result, we have to include the next variable in the
	     solution as well.  Otherwise two increments by offset / 2
	     do not result in the same or a conservative superset
	     solution.  */
	  if (temp->offset != offset
	      && temp->next != 0)
	    {
	      struct constraint_expr c2;
	      c2.var = temp->next;
	      c2.type = ADDRESSOF;
	      c2.offset = 0;
	      results->safe_push (c2);
	    }
	  c.var = temp->id;
	  c.offset = 0;
	}
      else
	c.offset = rhsoffset;

      (*results)[j] = c;
    }
}


/* Given a COMPONENT_REF T, return the constraint_expr vector for it.
   If address_p is true the result will be taken its address of.
   If lhs_p is true then the constraint expression is assumed to be used
   as the lhs.  */

static void
get_constraint_for_component_ref (tree t, vec<ce_s> *results,
				  bool address_p, bool lhs_p)
{
  tree orig_t = t;
  HOST_WIDE_INT bitsize = -1;
  HOST_WIDE_INT bitmaxsize = -1;
  HOST_WIDE_INT bitpos;
  tree forzero;

  /* Some people like to do cute things like take the address of
     &0->a.b */
  forzero = t;
  while (handled_component_p (forzero)
	 || INDIRECT_REF_P (forzero)
	 || TREE_CODE (forzero) == MEM_REF)
    forzero = TREE_OPERAND (forzero, 0);

  if (CONSTANT_CLASS_P (forzero) && integer_zerop (forzero))
    {
      struct constraint_expr temp;

      temp.offset = 0;
      temp.var = integer_id;
      temp.type = SCALAR;
      results->safe_push (temp);
      return;
    }

  /* Handle type-punning through unions.  If we are extracting a pointer
     from a union via a possibly type-punning access that pointer
     points to anything, similar to a conversion of an integer to
     a pointer.  */
  if (!lhs_p)
    {
      tree u;
      for (u = t;
	   TREE_CODE (u) == COMPONENT_REF || TREE_CODE (u) == ARRAY_REF;
	   u = TREE_OPERAND (u, 0))
	if (TREE_CODE (u) == COMPONENT_REF
	    && TREE_CODE (TREE_TYPE (TREE_OPERAND (u, 0))) == UNION_TYPE)
	  {
	    struct constraint_expr temp;

	    temp.offset = 0;
	    temp.var = anything_id;
	    temp.type = ADDRESSOF;
	    results->safe_push (temp);
	    return;
	  }
    }

  t = get_ref_base_and_extent (t, &bitpos, &bitsize, &bitmaxsize);

  /* Pretend to take the address of the base, we'll take care of
     adding the required subset of sub-fields below.  */
  get_constraint_for_1 (t, results, true, lhs_p);
  gcc_assert (results->length () == 1);
  struct constraint_expr &result = results->last ();

  if (result.type == SCALAR
      && get_varinfo (result.var)->is_full_var)
    /* For single-field vars do not bother about the offset.  */
    result.offset = 0;
  else if (result.type == SCALAR)
    {
      /* In languages like C, you can access one past the end of an
	 array.  You aren't allowed to dereference it, so we can
	 ignore this constraint. When we handle pointer subtraction,
	 we may have to do something cute here.  */

      if ((unsigned HOST_WIDE_INT)bitpos < get_varinfo (result.var)->fullsize
	  && bitmaxsize != 0)
	{
	  /* It's also not true that the constraint will actually start at the
	     right offset, it may start in some padding.  We only care about
	     setting the constraint to the first actual field it touches, so
	     walk to find it.  */
	  struct constraint_expr cexpr = result;
	  varinfo_t curr;
	  results->pop ();
	  cexpr.offset = 0;
	  for (curr = get_varinfo (cexpr.var); curr; curr = vi_next (curr))
	    {
	      if (ranges_overlap_p (curr->offset, curr->size,
				    bitpos, bitmaxsize))
		{
		  cexpr.var = curr->id;
		  results->safe_push (cexpr);
		  if (address_p)
		    break;
		}
	    }
	  /* If we are going to take the address of this field then
	     to be able to compute reachability correctly add at least
	     the last field of the variable.  */
	  if (address_p && results->length () == 0)
	    {
	      curr = get_varinfo (cexpr.var);
	      while (curr->next != 0)
		curr = vi_next (curr);
	      cexpr.var = curr->id;
	      results->safe_push (cexpr);
	    }
	  else if (results->length () == 0)
	    /* Assert that we found *some* field there. The user couldn't be
	       accessing *only* padding.  */
	    /* Still the user could access one past the end of an array
	       embedded in a struct resulting in accessing *only* padding.  */
	    /* Or accessing only padding via type-punning to a type
	       that has a filed just in padding space.  */
	    {
	      cexpr.type = SCALAR;
	      cexpr.var = anything_id;
	      cexpr.offset = 0;
	      results->safe_push (cexpr);
	    }
	}
      else if (bitmaxsize == 0)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Access to zero-sized part of variable,"
		     "ignoring\n");
	}
      else
	if (dump_file && (dump_flags & TDF_DETAILS))
	  fprintf (dump_file, "Access to past the end of variable, ignoring\n");
    }
  else if (result.type == DEREF)
    {
      /* If we do not know exactly where the access goes say so.  Note
	 that only for non-structure accesses we know that we access
	 at most one subfiled of any variable.  */
      if (bitpos == -1
	  || bitsize != bitmaxsize
	  || AGGREGATE_TYPE_P (TREE_TYPE (orig_t))
	  || result.offset == UNKNOWN_OFFSET)
	result.offset = UNKNOWN_OFFSET;
      else
	result.offset += bitpos;
    }
  else if (result.type == ADDRESSOF)
    {
      /* We can end up here for component references on a
         VIEW_CONVERT_EXPR <>(&foobar).  */
      result.type = SCALAR;
      result.var = anything_id;
      result.offset = 0;
    }
  else
    gcc_unreachable ();
}


/* Dereference the constraint expression CONS, and return the result.
   DEREF (ADDRESSOF) = SCALAR
   DEREF (SCALAR) = DEREF
   DEREF (DEREF) = (temp = DEREF1; result = DEREF(temp))
   This is needed so that we can handle dereferencing DEREF constraints.  */

static void
do_deref (vec<ce_s> *constraints)
{
  struct constraint_expr *c;
  unsigned int i = 0;

  FOR_EACH_VEC_ELT (*constraints, i, c)
    {
      if (c->type == SCALAR)
	c->type = DEREF;
      else if (c->type == ADDRESSOF)
	c->type = SCALAR;
      else if (c->type == DEREF)
	{
	  struct constraint_expr tmplhs;
	  tmplhs = new_scalar_tmp_constraint_exp ("dereftmp");
	  process_constraint (new_constraint (tmplhs, *c));
	  c->var = tmplhs.var;
	}
      else
	gcc_unreachable ();
    }
}

/* Given a tree T, return the constraint expression for taking the
   address of it.  */

static void
get_constraint_for_address_of (tree t, vec<ce_s> *results)
{
  struct constraint_expr *c;
  unsigned int i;

  get_constraint_for_1 (t, results, true, true);

  FOR_EACH_VEC_ELT (*results, i, c)
    {
      if (c->type == DEREF)
	c->type = SCALAR;
      else
	c->type = ADDRESSOF;
    }
}

/* Given a tree T, return the constraint expression for it.  */

static void
get_constraint_for_1 (tree t, vec<ce_s> *results, bool address_p,
		      bool lhs_p)
{
  struct constraint_expr temp;

  /* x = integer is all glommed to a single variable, which doesn't
     point to anything by itself.  That is, of course, unless it is an
     integer constant being treated as a pointer, in which case, we
     will return that this is really the addressof anything.  This
     happens below, since it will fall into the default case. The only
     case we know something about an integer treated like a pointer is
     when it is the NULL pointer, and then we just say it points to
     NULL.

     Do not do that if -fno-delete-null-pointer-checks though, because
     in that case *NULL does not fail, so it _should_ alias *anything.
     It is not worth adding a new option or renaming the existing one,
     since this case is relatively obscure.  */
  if ((TREE_CODE (t) == INTEGER_CST
       && integer_zerop (t))
      /* The only valid CONSTRUCTORs in gimple with pointer typed
	 elements are zero-initializer.  But in IPA mode we also
	 process global initializers, so verify at least.  */
      || (TREE_CODE (t) == CONSTRUCTOR
	  && CONSTRUCTOR_NELTS (t) == 0))
    {
      if (flag_delete_null_pointer_checks)
	temp.var = nothing_id;
      else
	temp.var = nonlocal_id;
      temp.type = ADDRESSOF;
      temp.offset = 0;
      results->safe_push (temp);
      return;
    }

  /* String constants are read-only.  */
  if (TREE_CODE (t) == STRING_CST)
    {
      temp.var = readonly_id;
      temp.type = SCALAR;
      temp.offset = 0;
      results->safe_push (temp);
      return;
    }

  switch (TREE_CODE_CLASS (TREE_CODE (t)))
    {
    case tcc_expression:
      {
	switch (TREE_CODE (t))
	  {
	  case ADDR_EXPR:
	    get_constraint_for_address_of (TREE_OPERAND (t, 0), results);
	    return;
	  default:;
	  }
	break;
      }
    case tcc_reference:
      {
	switch (TREE_CODE (t))
	  {
	  case MEM_REF:
	    {
	      struct constraint_expr cs;
	      varinfo_t vi, curr;
	      get_constraint_for_ptr_offset (TREE_OPERAND (t, 0),
					     TREE_OPERAND (t, 1), results);
	      do_deref (results);

	      /* If we are not taking the address then make sure to process
		 all subvariables we might access.  */
	      if (address_p)
		return;

	      cs = results->last ();
	      if (cs.type == DEREF
		  && type_can_have_subvars (TREE_TYPE (t)))
		{
		  /* For dereferences this means we have to defer it
		     to solving time.  */
		  results->last ().offset = UNKNOWN_OFFSET;
		  return;
		}
	      if (cs.type != SCALAR)
		return;

	      vi = get_varinfo (cs.var);
	      curr = vi_next (vi);
	      if (!vi->is_full_var
		  && curr)
		{
		  unsigned HOST_WIDE_INT size;
		  if (host_integerp (TYPE_SIZE (TREE_TYPE (t)), 1))
		    size = TREE_INT_CST_LOW (TYPE_SIZE (TREE_TYPE (t)));
		  else
		    size = -1;
		  for (; curr; curr = vi_next (curr))
		    {
		      if (curr->offset - vi->offset < size)
			{
			  cs.var = curr->id;
			  results->safe_push (cs);
			}
		      else
			break;
		    }
		}
	      return;
	    }
	  case ARRAY_REF:
	  case ARRAY_RANGE_REF:
	  case COMPONENT_REF:
	    get_constraint_for_component_ref (t, results, address_p, lhs_p);
	    return;
	  case VIEW_CONVERT_EXPR:
	    get_constraint_for_1 (TREE_OPERAND (t, 0), results, address_p,
				  lhs_p);
	    return;
	  /* We are missing handling for TARGET_MEM_REF here.  */
	  default:;
	  }
	break;
      }
    case tcc_exceptional:
      {
	switch (TREE_CODE (t))
	  {
	  case SSA_NAME:
	    {
	      get_constraint_for_ssa_var (t, results, address_p);
	      return;
	    }
	  case CONSTRUCTOR:
	    {
	      unsigned int i;
	      tree val;
	      vec<ce_s> tmp = vNULL;
	      FOR_EACH_CONSTRUCTOR_VALUE (CONSTRUCTOR_ELTS (t), i, val)
		{
		  struct constraint_expr *rhsp;
		  unsigned j;
		  get_constraint_for_1 (val, &tmp, address_p, lhs_p);
		  FOR_EACH_VEC_ELT (tmp, j, rhsp)
		    results->safe_push (*rhsp);
		  tmp.truncate (0);
		}
	      tmp.release ();
	      /* We do not know whether the constructor was complete,
	         so technically we have to add &NOTHING or &ANYTHING
		 like we do for an empty constructor as well.  */
	      return;
	    }
	  default:;
	  }
	break;
      }
    case tcc_declaration:
      {
	get_constraint_for_ssa_var (t, results, address_p);
	return;
      }
    case tcc_constant:
      {
	/* We cannot refer to automatic variables through constants.  */ 
	temp.type = ADDRESSOF;
	temp.var = nonlocal_id;
	temp.offset = 0;
	results->safe_push (temp);
	return;
      }
    default:;
    }

  /* The default fallback is a constraint from anything.  */
  temp.type = ADDRESSOF;
  temp.var = anything_id;
  temp.offset = 0;
  results->safe_push (temp);
}

/* Given a gimple tree T, return the constraint expression vector for it.  */

static void
get_constraint_for (tree t, vec<ce_s> *results)
{
  gcc_assert (results->length () == 0);

  get_constraint_for_1 (t, results, false, true);
}

/* Given a gimple tree T, return the constraint expression vector for it
   to be used as the rhs of a constraint.  */

static void
get_constraint_for_rhs (tree t, vec<ce_s> *results)
{
  gcc_assert (results->length () == 0);

  get_constraint_for_1 (t, results, false, false);
}


/* Efficiently generates constraints from all entries in *RHSC to all
   entries in *LHSC.  */

static void
process_all_all_constraints (vec<ce_s> lhsc,
			     vec<ce_s> rhsc)
{
  struct constraint_expr *lhsp, *rhsp;
  unsigned i, j;

  if (lhsc.length () <= 1 || rhsc.length () <= 1)
    {
      FOR_EACH_VEC_ELT (lhsc, i, lhsp)
	FOR_EACH_VEC_ELT (rhsc, j, rhsp)
	  process_constraint (new_constraint (*lhsp, *rhsp));
    }
  else
    {
      struct constraint_expr tmp;
      tmp = new_scalar_tmp_constraint_exp ("allalltmp");
      FOR_EACH_VEC_ELT (rhsc, i, rhsp)
	process_constraint (new_constraint (tmp, *rhsp));
      FOR_EACH_VEC_ELT (lhsc, i, lhsp)
	process_constraint (new_constraint (*lhsp, tmp));
    }
}

/* Handle aggregate copies by expanding into copies of the respective
   fields of the structures.  */

static void
do_structure_copy (tree lhsop, tree rhsop)
{
  struct constraint_expr *lhsp, *rhsp;
  vec<ce_s> lhsc = vNULL;
  vec<ce_s> rhsc = vNULL;
  unsigned j;

  get_constraint_for (lhsop, &lhsc);
  get_constraint_for_rhs (rhsop, &rhsc);
  lhsp = &lhsc[0];
  rhsp = &rhsc[0];
  if (lhsp->type == DEREF
      || (lhsp->type == ADDRESSOF && lhsp->var == anything_id)
      || rhsp->type == DEREF)
    {
      if (lhsp->type == DEREF)
	{
	  gcc_assert (lhsc.length () == 1);
	  lhsp->offset = UNKNOWN_OFFSET;
	}
      if (rhsp->type == DEREF)
	{
	  gcc_assert (rhsc.length () == 1);
	  rhsp->offset = UNKNOWN_OFFSET;
	}
      process_all_all_constraints (lhsc, rhsc);
    }
  else if (lhsp->type == SCALAR
	   && (rhsp->type == SCALAR
	       || rhsp->type == ADDRESSOF))
    {
      HOST_WIDE_INT lhssize, lhsmaxsize, lhsoffset;
      HOST_WIDE_INT rhssize, rhsmaxsize, rhsoffset;
      unsigned k = 0;
      get_ref_base_and_extent (lhsop, &lhsoffset, &lhssize, &lhsmaxsize);
      get_ref_base_and_extent (rhsop, &rhsoffset, &rhssize, &rhsmaxsize);
      for (j = 0; lhsc.iterate (j, &lhsp);)
	{
	  varinfo_t lhsv, rhsv;
	  rhsp = &rhsc[k];
	  lhsv = get_varinfo (lhsp->var);
	  rhsv = get_varinfo (rhsp->var);
	  if (lhsv->may_have_pointers
	      && (lhsv->is_full_var
		  || rhsv->is_full_var
		  || ranges_overlap_p (lhsv->offset + rhsoffset, lhsv->size,
				       rhsv->offset + lhsoffset, rhsv->size)))
	    process_constraint (new_constraint (*lhsp, *rhsp));
	  if (!rhsv->is_full_var
	      && (lhsv->is_full_var
		  || (lhsv->offset + rhsoffset + lhsv->size
		      > rhsv->offset + lhsoffset + rhsv->size)))
	    {
	      ++k;
	      if (k >= rhsc.length ())
		break;
	    }
	  else
	    ++j;
	}
    }
  else
    gcc_unreachable ();

  lhsc.release ();
  rhsc.release ();
}

/* Create constraints ID = { rhsc }.  */

static void
make_constraints_to (unsigned id, vec<ce_s> rhsc)
{
  struct constraint_expr *c;
  struct constraint_expr includes;
  unsigned int j;

  includes.var = id;
  includes.offset = 0;
  includes.type = SCALAR;

  FOR_EACH_VEC_ELT (rhsc, j, c)
    process_constraint (new_constraint (includes, *c));
}

/* Create a constraint ID = OP.  */

static void
make_constraint_to (unsigned id, tree op)
{
  vec<ce_s> rhsc = vNULL;
  get_constraint_for_rhs (op, &rhsc);
  make_constraints_to (id, rhsc);
  rhsc.release ();
}

/* Create a constraint ID = &FROM.  */

static void
make_constraint_from (varinfo_t vi, int from)
{
  struct constraint_expr lhs, rhs;

  lhs.var = vi->id;
  lhs.offset = 0;
  lhs.type = SCALAR;

  rhs.var = from;
  rhs.offset = 0;
  rhs.type = ADDRESSOF;
  process_constraint (new_constraint (lhs, rhs));
}

/* Create a constraint ID = FROM.  */

static void
make_copy_constraint (varinfo_t vi, int from)
{
  struct constraint_expr lhs, rhs;

  lhs.var = vi->id;
  lhs.offset = 0;
  lhs.type = SCALAR;

  rhs.var = from;
  rhs.offset = 0;
  rhs.type = SCALAR;
  process_constraint (new_constraint (lhs, rhs));
}

/* Make constraints necessary to make OP escape.  */

static void
make_escape_constraint (tree op)
{
  make_constraint_to (escaped_id, op);
}

/* Add constraints to that the solution of VI is transitively closed.  */

static void
make_transitive_closure_constraints (varinfo_t vi)
{
  struct constraint_expr lhs, rhs;

  /* VAR = *VAR;  */
  lhs.type = SCALAR;
  lhs.var = vi->id;
  lhs.offset = 0;
  rhs.type = DEREF;
  rhs.var = vi->id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));

  /* VAR = VAR + UNKNOWN;  */
  lhs.type = SCALAR;
  lhs.var = vi->id;
  lhs.offset = 0;
  rhs.type = SCALAR;
  rhs.var = vi->id;
  rhs.offset = UNKNOWN_OFFSET;
  process_constraint (new_constraint (lhs, rhs));
}

/* Temporary storage for fake var decls.  */
struct obstack fake_var_decl_obstack;

/* Build a fake VAR_DECL acting as referrer to a DECL_UID.  */

static tree
build_fake_var_decl (tree type)
{
  tree decl = (tree) XOBNEW (&fake_var_decl_obstack, struct tree_var_decl);
  memset (decl, 0, sizeof (struct tree_var_decl));
  TREE_SET_CODE (decl, VAR_DECL);
  TREE_TYPE (decl) = type;
  DECL_UID (decl) = allocate_decl_uid ();
  SET_DECL_PT_UID (decl, -1);
  layout_decl (decl, 0);
  return decl;
}

/* Create a new artificial heap variable with NAME.
   Return the created variable.  */

static varinfo_t
make_heapvar (const char *name)
{
  varinfo_t vi;
  tree heapvar;
  
  heapvar = build_fake_var_decl (ptr_type_node);
  DECL_EXTERNAL (heapvar) = 1;

  vi = new_var_info (heapvar, name);
  vi->is_artificial_var = true;
  vi->is_heap_var = true;
  vi->is_unknown_size_var = true;
  vi->offset = 0;
  vi->fullsize = ~0;
  vi->size = ~0;
  vi->is_full_var = true;
  insert_vi_for_tree (heapvar, vi);

  return vi;
}

/* Create a new artificial heap variable with NAME and make a
   constraint from it to LHS.  Set flags according to a tag used
   for tracking restrict pointers.  */

static varinfo_t
make_constraint_from_restrict (varinfo_t lhs, const char *name)
{
  varinfo_t vi = make_heapvar (name);
  vi->is_global_var = 1;
  vi->may_have_pointers = 1;
  make_constraint_from (lhs, vi->id);
  return vi;
}

/* Create a new artificial heap variable with NAME and make a
   constraint from it to LHS.  Set flags according to a tag used
   for tracking restrict pointers and make the artificial heap
   point to global memory.  */

static varinfo_t
make_constraint_from_global_restrict (varinfo_t lhs, const char *name)
{
  varinfo_t vi = make_constraint_from_restrict (lhs, name);
  make_copy_constraint (vi, nonlocal_id);
  return vi;
}

/* In IPA mode there are varinfos for different aspects of reach
   function designator.  One for the points-to set of the return
   value, one for the variables that are clobbered by the function,
   one for its uses and one for each parameter (including a single
   glob for remaining variadic arguments).  */

enum { fi_clobbers = 1, fi_uses = 2,
       fi_static_chain = 3, fi_result = 4, fi_parm_base = 5 };

/* Get a constraint for the requested part of a function designator FI
   when operating in IPA mode.  */

static struct constraint_expr
get_function_part_constraint (varinfo_t fi, unsigned part)
{
  struct constraint_expr c;

  gcc_assert (in_ipa_mode);

  if (fi->id == anything_id)
    {
      /* ???  We probably should have a ANYFN special variable.  */
      c.var = anything_id;
      c.offset = 0;
      c.type = SCALAR;
    }
  else if (TREE_CODE (fi->decl) == FUNCTION_DECL)
    {
      varinfo_t ai = first_vi_for_offset (fi, part);
      if (ai)
	c.var = ai->id;
      else
	c.var = anything_id;
      c.offset = 0;
      c.type = SCALAR;
    }
  else
    {
      c.var = fi->id;
      c.offset = part;
      c.type = DEREF;
    }

  return c;
}

/* For non-IPA mode, generate constraints necessary for a call on the
   RHS.  */

static void
handle_rhs_call (gimple stmt, vec<ce_s> *results)
{
  struct constraint_expr rhsc;
  unsigned i;
  bool returns_uses = false;

  for (i = 0; i < gimple_call_num_args (stmt); ++i)
    {
      tree arg = gimple_call_arg (stmt, i);
      int flags = gimple_call_arg_flags (stmt, i);

      /* If the argument is not used we can ignore it.  */
      if (flags & EAF_UNUSED)
	continue;

      /* As we compute ESCAPED context-insensitive we do not gain
         any precision with just EAF_NOCLOBBER but not EAF_NOESCAPE
	 set.  The argument would still get clobbered through the
	 escape solution.  */
      if ((flags & EAF_NOCLOBBER)
	   && (flags & EAF_NOESCAPE))
	{
	  varinfo_t uses = get_call_use_vi (stmt);
	  if (!(flags & EAF_DIRECT))
	    {
	      varinfo_t tem = new_var_info (NULL_TREE, "callarg");
	      make_constraint_to (tem->id, arg);
	      make_transitive_closure_constraints (tem);
	      make_copy_constraint (uses, tem->id);
	    }
	  else
	    make_constraint_to (uses->id, arg);
	  returns_uses = true;
	}
      else if (flags & EAF_NOESCAPE)
	{
	  struct constraint_expr lhs, rhs;
	  varinfo_t uses = get_call_use_vi (stmt);
	  varinfo_t clobbers = get_call_clobber_vi (stmt);
	  varinfo_t tem = new_var_info (NULL_TREE, "callarg");
	  make_constraint_to (tem->id, arg);
	  if (!(flags & EAF_DIRECT))
	    make_transitive_closure_constraints (tem);
	  make_copy_constraint (uses, tem->id);
	  make_copy_constraint (clobbers, tem->id);
	  /* Add *tem = nonlocal, do not add *tem = callused as
	     EAF_NOESCAPE parameters do not escape to other parameters
	     and all other uses appear in NONLOCAL as well.  */
	  lhs.type = DEREF;
	  lhs.var = tem->id;
	  lhs.offset = 0;
	  rhs.type = SCALAR;
	  rhs.var = nonlocal_id;
	  rhs.offset = 0;
	  process_constraint (new_constraint (lhs, rhs));
	  returns_uses = true;
	}
      else
	make_escape_constraint (arg);
    }

  /* If we added to the calls uses solution make sure we account for
     pointers to it to be returned.  */
  if (returns_uses)
    {
      rhsc.var = get_call_use_vi (stmt)->id;
      rhsc.offset = 0;
      rhsc.type = SCALAR;
      results->safe_push (rhsc);
    }

  /* The static chain escapes as well.  */
  if (gimple_call_chain (stmt))
    make_escape_constraint (gimple_call_chain (stmt));

  /* And if we applied NRV the address of the return slot escapes as well.  */
  if (gimple_call_return_slot_opt_p (stmt)
      && gimple_call_lhs (stmt) != NULL_TREE
      && TREE_ADDRESSABLE (TREE_TYPE (gimple_call_lhs (stmt))))
    {
      vec<ce_s> tmpc = vNULL;
      struct constraint_expr lhsc, *c;
      get_constraint_for_address_of (gimple_call_lhs (stmt), &tmpc);
      lhsc.var = escaped_id;
      lhsc.offset = 0;
      lhsc.type = SCALAR;
      FOR_EACH_VEC_ELT (tmpc, i, c)
	process_constraint (new_constraint (lhsc, *c));
      tmpc.release ();
    }

  /* Regular functions return nonlocal memory.  */
  rhsc.var = nonlocal_id;
  rhsc.offset = 0;
  rhsc.type = SCALAR;
  results->safe_push (rhsc);
}

/* For non-IPA mode, generate constraints necessary for a call
   that returns a pointer and assigns it to LHS.  This simply makes
   the LHS point to global and escaped variables.  */

static void
handle_lhs_call (gimple stmt, tree lhs, int flags, vec<ce_s> rhsc,
		 tree fndecl)
{
  vec<ce_s> lhsc = vNULL;

  get_constraint_for (lhs, &lhsc);
  /* If the store is to a global decl make sure to
     add proper escape constraints.  */
  lhs = get_base_address (lhs);
  if (lhs
      && DECL_P (lhs)
      && is_global_var (lhs))
    {
      struct constraint_expr tmpc;
      tmpc.var = escaped_id;
      tmpc.offset = 0;
      tmpc.type = SCALAR;
      lhsc.safe_push (tmpc);
    }

  /* If the call returns an argument unmodified override the rhs
     constraints.  */
  flags = gimple_call_return_flags (stmt);
  if (flags & ERF_RETURNS_ARG
      && (flags & ERF_RETURN_ARG_MASK) < gimple_call_num_args (stmt))
    {
      tree arg;
      rhsc.create (0);
      arg = gimple_call_arg (stmt, flags & ERF_RETURN_ARG_MASK);
      get_constraint_for (arg, &rhsc);
      process_all_all_constraints (lhsc, rhsc);
      rhsc.release ();
    }
  else if (flags & ERF_NOALIAS)
    {
      varinfo_t vi;
      struct constraint_expr tmpc;
      rhsc.create (0);
      vi = make_heapvar ("HEAP");
      /* We delay marking allocated storage global until we know if
         it escapes.  */
      DECL_EXTERNAL (vi->decl) = 0;
      vi->is_global_var = 0;
      /* If this is not a real malloc call assume the memory was
	 initialized and thus may point to global memory.  All
	 builtin functions with the malloc attribute behave in a sane way.  */
      if (!fndecl
	  || DECL_BUILT_IN_CLASS (fndecl) != BUILT_IN_NORMAL)
	make_constraint_from (vi, nonlocal_id);
      tmpc.var = vi->id;
      tmpc.offset = 0;
      tmpc.type = ADDRESSOF;
      rhsc.safe_push (tmpc);
      process_all_all_constraints (lhsc, rhsc);
      rhsc.release ();
    }
  else
    process_all_all_constraints (lhsc, rhsc);

  lhsc.release ();
}

/* For non-IPA mode, generate constraints necessary for a call of a
   const function that returns a pointer in the statement STMT.  */

static void
handle_const_call (gimple stmt, vec<ce_s> *results)
{
  struct constraint_expr rhsc;
  unsigned int k;

  /* Treat nested const functions the same as pure functions as far
     as the static chain is concerned.  */
  if (gimple_call_chain (stmt))
    {
      varinfo_t uses = get_call_use_vi (stmt);
      make_transitive_closure_constraints (uses);
      make_constraint_to (uses->id, gimple_call_chain (stmt));
      rhsc.var = uses->id;
      rhsc.offset = 0;
      rhsc.type = SCALAR;
      results->safe_push (rhsc);
    }

  /* May return arguments.  */
  for (k = 0; k < gimple_call_num_args (stmt); ++k)
    {
      tree arg = gimple_call_arg (stmt, k);
      vec<ce_s> argc = vNULL;
      unsigned i;
      struct constraint_expr *argp;
      get_constraint_for_rhs (arg, &argc);
      FOR_EACH_VEC_ELT (argc, i, argp)
	results->safe_push (*argp);
      argc.release ();
    }

  /* May return addresses of globals.  */
  rhsc.var = nonlocal_id;
  rhsc.offset = 0;
  rhsc.type = ADDRESSOF;
  results->safe_push (rhsc);
}

/* For non-IPA mode, generate constraints necessary for a call to a
   pure function in statement STMT.  */

static void
handle_pure_call (gimple stmt, vec<ce_s> *results)
{
  struct constraint_expr rhsc;
  unsigned i;
  varinfo_t uses = NULL;

  /* Memory reached from pointer arguments is call-used.  */
  for (i = 0; i < gimple_call_num_args (stmt); ++i)
    {
      tree arg = gimple_call_arg (stmt, i);
      if (!uses)
	{
	  uses = get_call_use_vi (stmt);
	  make_transitive_closure_constraints (uses);
	}
      make_constraint_to (uses->id, arg);
    }

  /* The static chain is used as well.  */
  if (gimple_call_chain (stmt))
    {
      if (!uses)
	{
	  uses = get_call_use_vi (stmt);
	  make_transitive_closure_constraints (uses);
	}
      make_constraint_to (uses->id, gimple_call_chain (stmt));
    }

  /* Pure functions may return call-used and nonlocal memory.  */
  if (uses)
    {
      rhsc.var = uses->id;
      rhsc.offset = 0;
      rhsc.type = SCALAR;
      results->safe_push (rhsc);
    }
  rhsc.var = nonlocal_id;
  rhsc.offset = 0;
  rhsc.type = SCALAR;
  results->safe_push (rhsc);
}


/* Return the varinfo for the callee of CALL.  */

static varinfo_t
get_fi_for_callee (gimple call)
{
  tree decl, fn = gimple_call_fn (call);

  if (fn && TREE_CODE (fn) == OBJ_TYPE_REF)
    fn = OBJ_TYPE_REF_EXPR (fn);

  /* If we can directly resolve the function being called, do so.
     Otherwise, it must be some sort of indirect expression that
     we should still be able to handle.  */
  decl = gimple_call_addr_fndecl (fn);
  if (decl)
    return get_vi_for_tree (decl);

  /* If the function is anything other than a SSA name pointer we have no
     clue and should be getting ANYFN (well, ANYTHING for now).  */
  if (!fn || TREE_CODE (fn) != SSA_NAME)
    return get_varinfo (anything_id);

  if (SSA_NAME_IS_DEFAULT_DEF (fn)
      && (TREE_CODE (SSA_NAME_VAR (fn)) == PARM_DECL
	  || TREE_CODE (SSA_NAME_VAR (fn)) == RESULT_DECL))
    fn = SSA_NAME_VAR (fn);

  return get_vi_for_tree (fn);
}

/* Create constraints for the builtin call T.  Return true if the call
   was handled, otherwise false.  */

static bool
find_func_aliases_for_builtin_call (gimple t)
{
  tree fndecl = gimple_call_fndecl (t);
  vec<ce_s> lhsc = vNULL;
  vec<ce_s> rhsc = vNULL;
  varinfo_t fi;

  if (gimple_call_builtin_p (t, BUILT_IN_NORMAL))
    /* ???  All builtins that are handled here need to be handled
       in the alias-oracle query functions explicitly!  */
    switch (DECL_FUNCTION_CODE (fndecl))
      {
      /* All the following functions return a pointer to the same object
	 as their first argument points to.  The functions do not add
	 to the ESCAPED solution.  The functions make the first argument
	 pointed to memory point to what the second argument pointed to
	 memory points to.  */
      case BUILT_IN_STRCPY:
      case BUILT_IN_STRNCPY:
      case BUILT_IN_BCOPY:
      case BUILT_IN_MEMCPY:
      case BUILT_IN_MEMMOVE:
      case BUILT_IN_MEMPCPY:
      case BUILT_IN_STPCPY:
      case BUILT_IN_STPNCPY:
      case BUILT_IN_STRCAT:
      case BUILT_IN_STRNCAT:
      case BUILT_IN_STRCPY_CHK:
      case BUILT_IN_STRNCPY_CHK:
      case BUILT_IN_MEMCPY_CHK:
      case BUILT_IN_MEMMOVE_CHK:
      case BUILT_IN_MEMPCPY_CHK:
      case BUILT_IN_STPCPY_CHK:
      case BUILT_IN_STPNCPY_CHK:
      case BUILT_IN_STRCAT_CHK:
      case BUILT_IN_STRNCAT_CHK:
      case BUILT_IN_TM_MEMCPY:
      case BUILT_IN_TM_MEMMOVE:
	{
	  tree res = gimple_call_lhs (t);
	  tree dest = gimple_call_arg (t, (DECL_FUNCTION_CODE (fndecl)
					   == BUILT_IN_BCOPY ? 1 : 0));
	  tree src = gimple_call_arg (t, (DECL_FUNCTION_CODE (fndecl)
					  == BUILT_IN_BCOPY ? 0 : 1));
	  if (res != NULL_TREE)
	    {
	      get_constraint_for (res, &lhsc);
	      if (DECL_FUNCTION_CODE (fndecl) == BUILT_IN_MEMPCPY
		  || DECL_FUNCTION_CODE (fndecl) == BUILT_IN_STPCPY
		  || DECL_FUNCTION_CODE (fndecl) == BUILT_IN_STPNCPY
		  || DECL_FUNCTION_CODE (fndecl) == BUILT_IN_MEMPCPY_CHK
		  || DECL_FUNCTION_CODE (fndecl) == BUILT_IN_STPCPY_CHK
		  || DECL_FUNCTION_CODE (fndecl) == BUILT_IN_STPNCPY_CHK)
		get_constraint_for_ptr_offset (dest, NULL_TREE, &rhsc);
	      else
		get_constraint_for (dest, &rhsc);
	      process_all_all_constraints (lhsc, rhsc);
	      lhsc.release ();
	      rhsc.release ();
	    }
	  get_constraint_for_ptr_offset (dest, NULL_TREE, &lhsc);
	  get_constraint_for_ptr_offset (src, NULL_TREE, &rhsc);
	  do_deref (&lhsc);
	  do_deref (&rhsc);
	  process_all_all_constraints (lhsc, rhsc);
	  lhsc.release ();
	  rhsc.release ();
	  return true;
	}
      case BUILT_IN_MEMSET:
      case BUILT_IN_MEMSET_CHK:
      case BUILT_IN_TM_MEMSET:
	{
	  tree res = gimple_call_lhs (t);
	  tree dest = gimple_call_arg (t, 0);
	  unsigned i;
	  ce_s *lhsp;
	  struct constraint_expr ac;
	  if (res != NULL_TREE)
	    {
	      get_constraint_for (res, &lhsc);
	      get_constraint_for (dest, &rhsc);
	      process_all_all_constraints (lhsc, rhsc);
	      lhsc.release ();
	      rhsc.release ();
	    }
	  get_constraint_for_ptr_offset (dest, NULL_TREE, &lhsc);
	  do_deref (&lhsc);
	  if (flag_delete_null_pointer_checks
	      && integer_zerop (gimple_call_arg (t, 1)))
	    {
	      ac.type = ADDRESSOF;
	      ac.var = nothing_id;
	    }
	  else
	    {
	      ac.type = SCALAR;
	      ac.var = integer_id;
	    }
	  ac.offset = 0;
	  FOR_EACH_VEC_ELT (lhsc, i, lhsp)
	      process_constraint (new_constraint (*lhsp, ac));
	  lhsc.release ();
	  return true;
	}
      case BUILT_IN_ASSUME_ALIGNED:
	{
	  tree res = gimple_call_lhs (t);
	  tree dest = gimple_call_arg (t, 0);
	  if (res != NULL_TREE)
	    {
	      get_constraint_for (res, &lhsc);
	      get_constraint_for (dest, &rhsc);
	      process_all_all_constraints (lhsc, rhsc);
	      lhsc.release ();
	      rhsc.release ();
	    }
	  return true;
	}
      /* All the following functions do not return pointers, do not
	 modify the points-to sets of memory reachable from their
	 arguments and do not add to the ESCAPED solution.  */
      case BUILT_IN_SINCOS:
      case BUILT_IN_SINCOSF:
      case BUILT_IN_SINCOSL:
      case BUILT_IN_FREXP:
      case BUILT_IN_FREXPF:
      case BUILT_IN_FREXPL:
      case BUILT_IN_GAMMA_R:
      case BUILT_IN_GAMMAF_R:
      case BUILT_IN_GAMMAL_R:
      case BUILT_IN_LGAMMA_R:
      case BUILT_IN_LGAMMAF_R:
      case BUILT_IN_LGAMMAL_R:
      case BUILT_IN_MODF:
      case BUILT_IN_MODFF:
      case BUILT_IN_MODFL:
      case BUILT_IN_REMQUO:
      case BUILT_IN_REMQUOF:
      case BUILT_IN_REMQUOL:
      case BUILT_IN_FREE:
	return true;
      case BUILT_IN_STRDUP:
      case BUILT_IN_STRNDUP:
	if (gimple_call_lhs (t))
	  {
	    handle_lhs_call (t, gimple_call_lhs (t), gimple_call_flags (t),
			     vNULL, fndecl);
	    get_constraint_for_ptr_offset (gimple_call_lhs (t),
					   NULL_TREE, &lhsc);
	    get_constraint_for_ptr_offset (gimple_call_arg (t, 0),
					   NULL_TREE, &rhsc);
	    do_deref (&lhsc);
	    do_deref (&rhsc);
	    process_all_all_constraints (lhsc, rhsc);
	    lhsc.release ();
	    rhsc.release ();
	    return true;
	  }
	break;
      /* String / character search functions return a pointer into the
         source string or NULL.  */
      case BUILT_IN_INDEX:
      case BUILT_IN_STRCHR:
      case BUILT_IN_STRRCHR:
      case BUILT_IN_MEMCHR:
      case BUILT_IN_STRSTR:
      case BUILT_IN_STRPBRK:
	if (gimple_call_lhs (t))
	  {
	    tree src = gimple_call_arg (t, 0);
	    get_constraint_for_ptr_offset (src, NULL_TREE, &rhsc);
	    constraint_expr nul;
	    nul.var = nothing_id;
	    nul.offset = 0;
	    nul.type = ADDRESSOF;
	    rhsc.safe_push (nul);
	    get_constraint_for (gimple_call_lhs (t), &lhsc);
	    process_all_all_constraints (lhsc, rhsc);
	    lhsc.release();
	    rhsc.release();
	  }
	return true;
      /* Trampolines are special - they set up passing the static
	 frame.  */
      case BUILT_IN_INIT_TRAMPOLINE:
	{
	  tree tramp = gimple_call_arg (t, 0);
	  tree nfunc = gimple_call_arg (t, 1);
	  tree frame = gimple_call_arg (t, 2);
	  unsigned i;
	  struct constraint_expr lhs, *rhsp;
	  if (in_ipa_mode)
	    {
	      varinfo_t nfi = NULL;
	      gcc_assert (TREE_CODE (nfunc) == ADDR_EXPR);
	      nfi = lookup_vi_for_tree (TREE_OPERAND (nfunc, 0));
	      if (nfi)
		{
		  lhs = get_function_part_constraint (nfi, fi_static_chain);
		  get_constraint_for (frame, &rhsc);
		  FOR_EACH_VEC_ELT (rhsc, i, rhsp)
		      process_constraint (new_constraint (lhs, *rhsp));
		  rhsc.release ();

		  /* Make the frame point to the function for
		     the trampoline adjustment call.  */
		  get_constraint_for (tramp, &lhsc);
		  do_deref (&lhsc);
		  get_constraint_for (nfunc, &rhsc);
		  process_all_all_constraints (lhsc, rhsc);
		  rhsc.release ();
		  lhsc.release ();

		  return true;
		}
	    }
	  /* Else fallthru to generic handling which will let
	     the frame escape.  */
	  break;
	}
      case BUILT_IN_ADJUST_TRAMPOLINE:
	{
	  tree tramp = gimple_call_arg (t, 0);
	  tree res = gimple_call_lhs (t);
	  if (in_ipa_mode && res)
	    {
	      get_constraint_for (res, &lhsc);
	      get_constraint_for (tramp, &rhsc);
	      do_deref (&rhsc);
	      process_all_all_constraints (lhsc, rhsc);
	      rhsc.release ();
	      lhsc.release ();
	    }
	  return true;
	}
      CASE_BUILT_IN_TM_STORE (1):
      CASE_BUILT_IN_TM_STORE (2):
      CASE_BUILT_IN_TM_STORE (4):
      CASE_BUILT_IN_TM_STORE (8):
      CASE_BUILT_IN_TM_STORE (FLOAT):
      CASE_BUILT_IN_TM_STORE (DOUBLE):
      CASE_BUILT_IN_TM_STORE (LDOUBLE):
      CASE_BUILT_IN_TM_STORE (M64):
      CASE_BUILT_IN_TM_STORE (M128):
      CASE_BUILT_IN_TM_STORE (M256):
	{
	  tree addr = gimple_call_arg (t, 0);
	  tree src = gimple_call_arg (t, 1);

	  get_constraint_for (addr, &lhsc);
	  do_deref (&lhsc);
	  get_constraint_for (src, &rhsc);
	  process_all_all_constraints (lhsc, rhsc);
	  lhsc.release ();
	  rhsc.release ();
	  return true;
	}
      CASE_BUILT_IN_TM_LOAD (1):
      CASE_BUILT_IN_TM_LOAD (2):
      CASE_BUILT_IN_TM_LOAD (4):
      CASE_BUILT_IN_TM_LOAD (8):
      CASE_BUILT_IN_TM_LOAD (FLOAT):
      CASE_BUILT_IN_TM_LOAD (DOUBLE):
      CASE_BUILT_IN_TM_LOAD (LDOUBLE):
      CASE_BUILT_IN_TM_LOAD (M64):
      CASE_BUILT_IN_TM_LOAD (M128):
      CASE_BUILT_IN_TM_LOAD (M256):
	{
	  tree dest = gimple_call_lhs (t);
	  tree addr = gimple_call_arg (t, 0);

	  get_constraint_for (dest, &lhsc);
	  get_constraint_for (addr, &rhsc);
	  do_deref (&rhsc);
	  process_all_all_constraints (lhsc, rhsc);
	  lhsc.release ();
	  rhsc.release ();
	  return true;
	}
      /* Variadic argument handling needs to be handled in IPA
	 mode as well.  */
      case BUILT_IN_VA_START:
	{
	  tree valist = gimple_call_arg (t, 0);
	  struct constraint_expr rhs, *lhsp;
	  unsigned i;
	  get_constraint_for (valist, &lhsc);
	  do_deref (&lhsc);
	  /* The va_list gets access to pointers in variadic
	     arguments.  Which we know in the case of IPA analysis
	     and otherwise are just all nonlocal variables.  */
	  if (in_ipa_mode)
	    {
	      fi = lookup_vi_for_tree (cfun->decl);
	      rhs = get_function_part_constraint (fi, ~0);
	      rhs.type = ADDRESSOF;
	    }
	  else
	    {
	      rhs.var = nonlocal_id;
	      rhs.type = ADDRESSOF;
	      rhs.offset = 0;
	    }
	  FOR_EACH_VEC_ELT (lhsc, i, lhsp)
	    process_constraint (new_constraint (*lhsp, rhs));
	  lhsc.release ();
	  /* va_list is clobbered.  */
	  make_constraint_to (get_call_clobber_vi (t)->id, valist);
	  return true;
	}
      /* va_end doesn't have any effect that matters.  */
      case BUILT_IN_VA_END:
	return true;
      /* Alternate return.  Simply give up for now.  */
      case BUILT_IN_RETURN:
	{
	  fi = NULL;
	  if (!in_ipa_mode
	      || !(fi = get_vi_for_tree (cfun->decl)))
	    make_constraint_from (get_varinfo (escaped_id), anything_id);
	  else if (in_ipa_mode
		   && fi != NULL)
	    {
	      struct constraint_expr lhs, rhs;
	      lhs = get_function_part_constraint (fi, fi_result);
	      rhs.var = anything_id;
	      rhs.offset = 0;
	      rhs.type = SCALAR;
	      process_constraint (new_constraint (lhs, rhs));
	    }
	  return true;
	}
      /* printf-style functions may have hooks to set pointers to
	 point to somewhere into the generated string.  Leave them
	 for a later exercise...  */
      default:
	/* Fallthru to general call handling.  */;
      }

  return false;
}

/* Create constraints for the call T.  */

static void
find_func_aliases_for_call (gimple t)
{
  tree fndecl = gimple_call_fndecl (t);
  vec<ce_s> lhsc = vNULL;
  vec<ce_s> rhsc = vNULL;
  varinfo_t fi;

  if (fndecl != NULL_TREE
      && DECL_BUILT_IN (fndecl)
      && find_func_aliases_for_builtin_call (t))
    return;

  fi = get_fi_for_callee (t);
  if (!in_ipa_mode
      || (fndecl && !fi->is_fn_info))
    {
      vec<ce_s> rhsc = vNULL;
      int flags = gimple_call_flags (t);

      /* Const functions can return their arguments and addresses
	 of global memory but not of escaped memory.  */
      if (flags & (ECF_CONST|ECF_NOVOPS))
	{
	  if (gimple_call_lhs (t))
	    handle_const_call (t, &rhsc);
	}
      /* Pure functions can return addresses in and of memory
	 reachable from their arguments, but they are not an escape
	 point for reachable memory of their arguments.  */
      else if (flags & (ECF_PURE|ECF_LOOPING_CONST_OR_PURE))
	handle_pure_call (t, &rhsc);
      else
	handle_rhs_call (t, &rhsc);
      if (gimple_call_lhs (t))
	handle_lhs_call (t, gimple_call_lhs (t), flags, rhsc, fndecl);
      rhsc.release ();
    }
  else
    {
      tree lhsop;
      unsigned j;

      /* Assign all the passed arguments to the appropriate incoming
	 parameters of the function.  */
      for (j = 0; j < gimple_call_num_args (t); j++)
	{
	  struct constraint_expr lhs ;
	  struct constraint_expr *rhsp;
	  tree arg = gimple_call_arg (t, j);

	  get_constraint_for_rhs (arg, &rhsc);
	  lhs = get_function_part_constraint (fi, fi_parm_base + j);
	  while (rhsc.length () != 0)
	    {
	      rhsp = &rhsc.last ();
	      process_constraint (new_constraint (lhs, *rhsp));
	      rhsc.pop ();
	    }
	}

      /* If we are returning a value, assign it to the result.  */
      lhsop = gimple_call_lhs (t);
      if (lhsop)
	{
	  struct constraint_expr rhs;
	  struct constraint_expr *lhsp;

	  get_constraint_for (lhsop, &lhsc);
	  rhs = get_function_part_constraint (fi, fi_result);
	  if (fndecl
	      && DECL_RESULT (fndecl)
	      && DECL_BY_REFERENCE (DECL_RESULT (fndecl)))
	    {
	      vec<ce_s> tem = vNULL;
	      tem.safe_push (rhs);
	      do_deref (&tem);
	      rhs = tem[0];
	      tem.release ();
	    }
	  FOR_EACH_VEC_ELT (lhsc, j, lhsp)
	    process_constraint (new_constraint (*lhsp, rhs));
	}

      /* If we pass the result decl by reference, honor that.  */
      if (lhsop
	  && fndecl
	  && DECL_RESULT (fndecl)
	  && DECL_BY_REFERENCE (DECL_RESULT (fndecl)))
	{
	  struct constraint_expr lhs;
	  struct constraint_expr *rhsp;

	  get_constraint_for_address_of (lhsop, &rhsc);
	  lhs = get_function_part_constraint (fi, fi_result);
	  FOR_EACH_VEC_ELT (rhsc, j, rhsp)
	    process_constraint (new_constraint (lhs, *rhsp));
	  rhsc.release ();
	}

      /* If we use a static chain, pass it along.  */
      if (gimple_call_chain (t))
	{
	  struct constraint_expr lhs;
	  struct constraint_expr *rhsp;

	  get_constraint_for (gimple_call_chain (t), &rhsc);
	  lhs = get_function_part_constraint (fi, fi_static_chain);
	  FOR_EACH_VEC_ELT (rhsc, j, rhsp)
	    process_constraint (new_constraint (lhs, *rhsp));
	}
    }
}

/* Walk statement T setting up aliasing constraints according to the
   references found in T.  This function is the main part of the
   constraint builder.  AI points to auxiliary alias information used
   when building alias sets and computing alias grouping heuristics.  */

static void
find_func_aliases (gimple origt)
{
  gimple t = origt;
  vec<ce_s> lhsc = vNULL;
  vec<ce_s> rhsc = vNULL;
  struct constraint_expr *c;
  varinfo_t fi;

  /* Now build constraints expressions.  */
  if (gimple_code (t) == GIMPLE_PHI)
    {
      size_t i;
      unsigned int j;

      /* For a phi node, assign all the arguments to
	 the result.  */
      get_constraint_for (gimple_phi_result (t), &lhsc);
      for (i = 0; i < gimple_phi_num_args (t); i++)
	{
	  tree strippedrhs = PHI_ARG_DEF (t, i);

	  STRIP_NOPS (strippedrhs);
	  get_constraint_for_rhs (gimple_phi_arg_def (t, i), &rhsc);

	  FOR_EACH_VEC_ELT (lhsc, j, c)
	    {
	      struct constraint_expr *c2;
	      while (rhsc.length () > 0)
		{
		  c2 = &rhsc.last ();
		  process_constraint (new_constraint (*c, *c2));
		  rhsc.pop ();
		}
	    }
	}
    }
  /* In IPA mode, we need to generate constraints to pass call
     arguments through their calls.   There are two cases,
     either a GIMPLE_CALL returning a value, or just a plain
     GIMPLE_CALL when we are not.

     In non-ipa mode, we need to generate constraints for each
     pointer passed by address.  */
  else if (is_gimple_call (t))
    find_func_aliases_for_call (t);
    
  /* Otherwise, just a regular assignment statement.  Only care about
     operations with pointer result, others are dealt with as escape
     points if they have pointer operands.  */
  else if (is_gimple_assign (t))
    {
      /* Otherwise, just a regular assignment statement.  */
      tree lhsop = gimple_assign_lhs (t);
      tree rhsop = (gimple_num_ops (t) == 2) ? gimple_assign_rhs1 (t) : NULL;

      if (rhsop && TREE_CLOBBER_P (rhsop))
	/* Ignore clobbers, they don't actually store anything into
	   the LHS.  */
	;
      else if (rhsop && AGGREGATE_TYPE_P (TREE_TYPE (lhsop)))
	do_structure_copy (lhsop, rhsop);
      else
	{
	  enum tree_code code = gimple_assign_rhs_code (t);

	  get_constraint_for (lhsop, &lhsc);

	  if (FLOAT_TYPE_P (TREE_TYPE (lhsop)))
	    /* If the operation produces a floating point result then
	       assume the value is not produced to transfer a pointer.  */
	    ;
	  else if (code == POINTER_PLUS_EXPR)
	    get_constraint_for_ptr_offset (gimple_assign_rhs1 (t),
					   gimple_assign_rhs2 (t), &rhsc);
	  else if (code == BIT_AND_EXPR
		   && TREE_CODE (gimple_assign_rhs2 (t)) == INTEGER_CST)
	    {
	      /* Aligning a pointer via a BIT_AND_EXPR is offsetting
		 the pointer.  Handle it by offsetting it by UNKNOWN.  */
	      get_constraint_for_ptr_offset (gimple_assign_rhs1 (t),
					     NULL_TREE, &rhsc);
	    }
	  else if ((CONVERT_EXPR_CODE_P (code)
		    && !(POINTER_TYPE_P (gimple_expr_type (t))
			 && !POINTER_TYPE_P (TREE_TYPE (rhsop))))
		   || gimple_assign_single_p (t))
	    get_constraint_for_rhs (rhsop, &rhsc);
	  else if (code == COND_EXPR)
	    {
	      /* The result is a merge of both COND_EXPR arms.  */
	      vec<ce_s> tmp = vNULL;
	      struct constraint_expr *rhsp;
	      unsigned i;
	      get_constraint_for_rhs (gimple_assign_rhs2 (t), &rhsc);
	      get_constraint_for_rhs (gimple_assign_rhs3 (t), &tmp);
	      FOR_EACH_VEC_ELT (tmp, i, rhsp)
		rhsc.safe_push (*rhsp);
	      tmp.release ();
	    }
	  else if (truth_value_p (code))
	    /* Truth value results are not pointer (parts).  Or at least
	       very very unreasonable obfuscation of a part.  */
	    ;
	  else
	    {
	      /* All other operations are merges.  */
	      vec<ce_s> tmp = vNULL;
	      struct constraint_expr *rhsp;
	      unsigned i, j;
	      get_constraint_for_rhs (gimple_assign_rhs1 (t), &rhsc);
	      for (i = 2; i < gimple_num_ops (t); ++i)
		{
		  get_constraint_for_rhs (gimple_op (t, i), &tmp);
		  FOR_EACH_VEC_ELT (tmp, j, rhsp)
		    rhsc.safe_push (*rhsp);
		  tmp.truncate (0);
		}
	      tmp.release ();
	    }
	  process_all_all_constraints (lhsc, rhsc);
	}
      /* If there is a store to a global variable the rhs escapes.  */
      if ((lhsop = get_base_address (lhsop)) != NULL_TREE
	  && DECL_P (lhsop)
	  && is_global_var (lhsop)
	  && (!in_ipa_mode
	      || DECL_EXTERNAL (lhsop) || TREE_PUBLIC (lhsop)))
	make_escape_constraint (rhsop);
    }
  /* Handle escapes through return.  */
  else if (gimple_code (t) == GIMPLE_RETURN
	   && gimple_return_retval (t) != NULL_TREE)
    {
      fi = NULL;
      if (!in_ipa_mode
	  || !(fi = get_vi_for_tree (cfun->decl)))
	make_escape_constraint (gimple_return_retval (t));
      else if (in_ipa_mode
	       && fi != NULL)
	{
	  struct constraint_expr lhs ;
	  struct constraint_expr *rhsp;
	  unsigned i;

	  lhs = get_function_part_constraint (fi, fi_result);
	  get_constraint_for_rhs (gimple_return_retval (t), &rhsc);
	  FOR_EACH_VEC_ELT (rhsc, i, rhsp)
	    process_constraint (new_constraint (lhs, *rhsp));
	}
    }
  /* Handle asms conservatively by adding escape constraints to everything.  */
  else if (gimple_code (t) == GIMPLE_ASM)
    {
      unsigned i, noutputs;
      const char **oconstraints;
      const char *constraint;
      bool allows_mem, allows_reg, is_inout;

      noutputs = gimple_asm_noutputs (t);
      oconstraints = XALLOCAVEC (const char *, noutputs);

      for (i = 0; i < noutputs; ++i)
	{
	  tree link = gimple_asm_output_op (t, i);
	  tree op = TREE_VALUE (link);

	  constraint = TREE_STRING_POINTER (TREE_VALUE (TREE_PURPOSE (link)));
	  oconstraints[i] = constraint;
	  parse_output_constraint (&constraint, i, 0, 0, &allows_mem,
				   &allows_reg, &is_inout);

	  /* A memory constraint makes the address of the operand escape.  */
	  if (!allows_reg && allows_mem)
	    make_escape_constraint (build_fold_addr_expr (op));

	  /* The asm may read global memory, so outputs may point to
	     any global memory.  */
	  if (op)
	    {
	      vec<ce_s> lhsc = vNULL;
	      struct constraint_expr rhsc, *lhsp;
	      unsigned j;
	      get_constraint_for (op, &lhsc);
	      rhsc.var = nonlocal_id;
	      rhsc.offset = 0;
	      rhsc.type = SCALAR;
	      FOR_EACH_VEC_ELT (lhsc, j, lhsp)
		process_constraint (new_constraint (*lhsp, rhsc));
	      lhsc.release ();
	    }
	}
      for (i = 0; i < gimple_asm_ninputs (t); ++i)
	{
	  tree link = gimple_asm_input_op (t, i);
	  tree op = TREE_VALUE (link);

	  constraint = TREE_STRING_POINTER (TREE_VALUE (TREE_PURPOSE (link)));

	  parse_input_constraint (&constraint, 0, 0, noutputs, 0, oconstraints,
				  &allows_mem, &allows_reg);

	  /* A memory constraint makes the address of the operand escape.  */
	  if (!allows_reg && allows_mem)
	    make_escape_constraint (build_fold_addr_expr (op));
	  /* Strictly we'd only need the constraint to ESCAPED if
	     the asm clobbers memory, otherwise using something
	     along the lines of per-call clobbers/uses would be enough.  */
	  else if (op)
	    make_escape_constraint (op);
	}
    }

  rhsc.release ();
  lhsc.release ();
}


/* Create a constraint adding to the clobber set of FI the memory
   pointed to by PTR.  */

static void
process_ipa_clobber (varinfo_t fi, tree ptr)
{
  vec<ce_s> ptrc = vNULL;
  struct constraint_expr *c, lhs;
  unsigned i;
  get_constraint_for_rhs (ptr, &ptrc);
  lhs = get_function_part_constraint (fi, fi_clobbers);
  FOR_EACH_VEC_ELT (ptrc, i, c)
    process_constraint (new_constraint (lhs, *c));
  ptrc.release ();
}

/* Walk statement T setting up clobber and use constraints according to the
   references found in T.  This function is a main part of the
   IPA constraint builder.  */

static void
find_func_clobbers (gimple origt)
{
  gimple t = origt;
  vec<ce_s> lhsc = vNULL;
  vec<ce_s> rhsc = vNULL;
  varinfo_t fi;

  /* Add constraints for clobbered/used in IPA mode.
     We are not interested in what automatic variables are clobbered
     or used as we only use the information in the caller to which
     they do not escape.  */
  gcc_assert (in_ipa_mode);

  /* If the stmt refers to memory in any way it better had a VUSE.  */
  if (gimple_vuse (t) == NULL_TREE)
    return;

  /* We'd better have function information for the current function.  */
  fi = lookup_vi_for_tree (cfun->decl);
  gcc_assert (fi != NULL);

  /* Account for stores in assignments and calls.  */
  if (gimple_vdef (t) != NULL_TREE
      && gimple_has_lhs (t))
    {
      tree lhs = gimple_get_lhs (t);
      tree tem = lhs;
      while (handled_component_p (tem))
	tem = TREE_OPERAND (tem, 0);
      if ((DECL_P (tem)
	   && !auto_var_in_fn_p (tem, cfun->decl))
	  || INDIRECT_REF_P (tem)
	  || (TREE_CODE (tem) == MEM_REF
	      && !(TREE_CODE (TREE_OPERAND (tem, 0)) == ADDR_EXPR
		   && auto_var_in_fn_p
		        (TREE_OPERAND (TREE_OPERAND (tem, 0), 0), cfun->decl))))
	{
	  struct constraint_expr lhsc, *rhsp;
	  unsigned i;
	  lhsc = get_function_part_constraint (fi, fi_clobbers);
	  get_constraint_for_address_of (lhs, &rhsc);
	  FOR_EACH_VEC_ELT (rhsc, i, rhsp)
	    process_constraint (new_constraint (lhsc, *rhsp));
	  rhsc.release ();
	}
    }

  /* Account for uses in assigments and returns.  */
  if (gimple_assign_single_p (t)
      || (gimple_code (t) == GIMPLE_RETURN
	  && gimple_return_retval (t) != NULL_TREE))
    {
      tree rhs = (gimple_assign_single_p (t)
		  ? gimple_assign_rhs1 (t) : gimple_return_retval (t));
      tree tem = rhs;
      while (handled_component_p (tem))
	tem = TREE_OPERAND (tem, 0);
      if ((DECL_P (tem)
	   && !auto_var_in_fn_p (tem, cfun->decl))
	  || INDIRECT_REF_P (tem)
	  || (TREE_CODE (tem) == MEM_REF
	      && !(TREE_CODE (TREE_OPERAND (tem, 0)) == ADDR_EXPR
		   && auto_var_in_fn_p
		        (TREE_OPERAND (TREE_OPERAND (tem, 0), 0), cfun->decl))))
	{
	  struct constraint_expr lhs, *rhsp;
	  unsigned i;
	  lhs = get_function_part_constraint (fi, fi_uses);
	  get_constraint_for_address_of (rhs, &rhsc);
	  FOR_EACH_VEC_ELT (rhsc, i, rhsp)
	    process_constraint (new_constraint (lhs, *rhsp));
	  rhsc.release ();
	}
    }

  if (is_gimple_call (t))
    {
      varinfo_t cfi = NULL;
      tree decl = gimple_call_fndecl (t);
      struct constraint_expr lhs, rhs;
      unsigned i, j;

      /* For builtins we do not have separate function info.  For those
	 we do not generate escapes for we have to generate clobbers/uses.  */
      if (gimple_call_builtin_p (t, BUILT_IN_NORMAL))
	switch (DECL_FUNCTION_CODE (decl))
	  {
	  /* The following functions use and clobber memory pointed to
	     by their arguments.  */
	  case BUILT_IN_STRCPY:
	  case BUILT_IN_STRNCPY:
	  case BUILT_IN_BCOPY:
	  case BUILT_IN_MEMCPY:
	  case BUILT_IN_MEMMOVE:
	  case BUILT_IN_MEMPCPY:
	  case BUILT_IN_STPCPY:
	  case BUILT_IN_STPNCPY:
	  case BUILT_IN_STRCAT:
	  case BUILT_IN_STRNCAT:
	  case BUILT_IN_STRCPY_CHK:
	  case BUILT_IN_STRNCPY_CHK:
	  case BUILT_IN_MEMCPY_CHK:
	  case BUILT_IN_MEMMOVE_CHK:
	  case BUILT_IN_MEMPCPY_CHK:
	  case BUILT_IN_STPCPY_CHK:
	  case BUILT_IN_STPNCPY_CHK:
	  case BUILT_IN_STRCAT_CHK:
	  case BUILT_IN_STRNCAT_CHK:
	    {
	      tree dest = gimple_call_arg (t, (DECL_FUNCTION_CODE (decl)
					       == BUILT_IN_BCOPY ? 1 : 0));
	      tree src = gimple_call_arg (t, (DECL_FUNCTION_CODE (decl)
					      == BUILT_IN_BCOPY ? 0 : 1));
	      unsigned i;
	      struct constraint_expr *rhsp, *lhsp;
	      get_constraint_for_ptr_offset (dest, NULL_TREE, &lhsc);
	      lhs = get_function_part_constraint (fi, fi_clobbers);
	      FOR_EACH_VEC_ELT (lhsc, i, lhsp)
		process_constraint (new_constraint (lhs, *lhsp));
	      lhsc.release ();
	      get_constraint_for_ptr_offset (src, NULL_TREE, &rhsc);
	      lhs = get_function_part_constraint (fi, fi_uses);
	      FOR_EACH_VEC_ELT (rhsc, i, rhsp)
		process_constraint (new_constraint (lhs, *rhsp));
	      rhsc.release ();
	      return;
	    }
	  /* The following function clobbers memory pointed to by
	     its argument.  */
	  case BUILT_IN_MEMSET:
	  case BUILT_IN_MEMSET_CHK:
	    {
	      tree dest = gimple_call_arg (t, 0);
	      unsigned i;
	      ce_s *lhsp;
	      get_constraint_for_ptr_offset (dest, NULL_TREE, &lhsc);
	      lhs = get_function_part_constraint (fi, fi_clobbers);
	      FOR_EACH_VEC_ELT (lhsc, i, lhsp)
		process_constraint (new_constraint (lhs, *lhsp));
	      lhsc.release ();
	      return;
	    }
	  /* The following functions clobber their second and third
	     arguments.  */
	  case BUILT_IN_SINCOS:
	  case BUILT_IN_SINCOSF:
	  case BUILT_IN_SINCOSL:
	    {
	      process_ipa_clobber (fi, gimple_call_arg (t, 1));
	      process_ipa_clobber (fi, gimple_call_arg (t, 2));
	      return;
	    }
	  /* The following functions clobber their second argument.  */
	  case BUILT_IN_FREXP:
	  case BUILT_IN_FREXPF:
	  case BUILT_IN_FREXPL:
	  case BUILT_IN_LGAMMA_R:
	  case BUILT_IN_LGAMMAF_R:
	  case BUILT_IN_LGAMMAL_R:
	  case BUILT_IN_GAMMA_R:
	  case BUILT_IN_GAMMAF_R:
	  case BUILT_IN_GAMMAL_R:
	  case BUILT_IN_MODF:
	  case BUILT_IN_MODFF:
	  case BUILT_IN_MODFL:
	    {
	      process_ipa_clobber (fi, gimple_call_arg (t, 1));
	      return;
	    }
	  /* The following functions clobber their third argument.  */
	  case BUILT_IN_REMQUO:
	  case BUILT_IN_REMQUOF:
	  case BUILT_IN_REMQUOL:
	    {
	      process_ipa_clobber (fi, gimple_call_arg (t, 2));
	      return;
	    }
	  /* The following functions neither read nor clobber memory.  */
	  case BUILT_IN_ASSUME_ALIGNED:
	  case BUILT_IN_FREE:
	    return;
	  /* Trampolines are of no interest to us.  */
	  case BUILT_IN_INIT_TRAMPOLINE:
	  case BUILT_IN_ADJUST_TRAMPOLINE:
	    return;
	  case BUILT_IN_VA_START:
	  case BUILT_IN_VA_END:
	    return;
	  /* printf-style functions may have hooks to set pointers to
	     point to somewhere into the generated string.  Leave them
	     for a later exercise...  */
	  default:
	    /* Fallthru to general call handling.  */;
	  }

      /* Parameters passed by value are used.  */
      lhs = get_function_part_constraint (fi, fi_uses);
      for (i = 0; i < gimple_call_num_args (t); i++)
	{
	  struct constraint_expr *rhsp;
	  tree arg = gimple_call_arg (t, i);

	  if (TREE_CODE (arg) == SSA_NAME
	      || is_gimple_min_invariant (arg))
	    continue;

	  get_constraint_for_address_of (arg, &rhsc);
	  FOR_EACH_VEC_ELT (rhsc, j, rhsp)
	    process_constraint (new_constraint (lhs, *rhsp));
	  rhsc.release ();
	}

      /* Build constraints for propagating clobbers/uses along the
	 callgraph edges.  */
      cfi = get_fi_for_callee (t);
      if (cfi->id == anything_id)
	{
	  if (gimple_vdef (t))
	    make_constraint_from (first_vi_for_offset (fi, fi_clobbers),
				  anything_id);
	  make_constraint_from (first_vi_for_offset (fi, fi_uses),
				anything_id);
	  return;
	}

      /* For callees without function info (that's external functions),
	 ESCAPED is clobbered and used.  */
      if (gimple_call_fndecl (t)
	  && !cfi->is_fn_info)
	{
	  varinfo_t vi;

	  if (gimple_vdef (t))
	    make_copy_constraint (first_vi_for_offset (fi, fi_clobbers),
				  escaped_id);
	  make_copy_constraint (first_vi_for_offset (fi, fi_uses), escaped_id);

	  /* Also honor the call statement use/clobber info.  */
	  if ((vi = lookup_call_clobber_vi (t)) != NULL)
	    make_copy_constraint (first_vi_for_offset (fi, fi_clobbers),
				  vi->id);
	  if ((vi = lookup_call_use_vi (t)) != NULL)
	    make_copy_constraint (first_vi_for_offset (fi, fi_uses),
				  vi->id);
	  return;
	}

      /* Otherwise the caller clobbers and uses what the callee does.
	 ???  This should use a new complex constraint that filters
	 local variables of the callee.  */
      if (gimple_vdef (t))
	{
	  lhs = get_function_part_constraint (fi, fi_clobbers);
	  rhs = get_function_part_constraint (cfi, fi_clobbers);
	  process_constraint (new_constraint (lhs, rhs));
	}
      lhs = get_function_part_constraint (fi, fi_uses);
      rhs = get_function_part_constraint (cfi, fi_uses);
      process_constraint (new_constraint (lhs, rhs));
    }
  else if (gimple_code (t) == GIMPLE_ASM)
    {
      /* ???  Ick.  We can do better.  */
      if (gimple_vdef (t))
	make_constraint_from (first_vi_for_offset (fi, fi_clobbers),
			      anything_id);
      make_constraint_from (first_vi_for_offset (fi, fi_uses),
			    anything_id);
    }

  rhsc.release ();
}


/* Find the first varinfo in the same variable as START that overlaps with
   OFFSET.  Return NULL if we can't find one.  */

static varinfo_t
first_vi_for_offset (varinfo_t start, unsigned HOST_WIDE_INT offset)
{
  /* If the offset is outside of the variable, bail out.  */
  if (offset >= start->fullsize)
    return NULL;

  /* If we cannot reach offset from start, lookup the first field
     and start from there.  */
  if (start->offset > offset)
    start = get_varinfo (start->head);

  while (start)
    {
      /* We may not find a variable in the field list with the actual
	 offset when when we have glommed a structure to a variable.
	 In that case, however, offset should still be within the size
	 of the variable. */
      if (offset >= start->offset
	  && (offset - start->offset) < start->size)
	return start;

      start = vi_next (start);
    }

  return NULL;
}

/* Find the first varinfo in the same variable as START that overlaps with
   OFFSET.  If there is no such varinfo the varinfo directly preceding
   OFFSET is returned.  */

static varinfo_t
first_or_preceding_vi_for_offset (varinfo_t start,
				  unsigned HOST_WIDE_INT offset)
{
  /* If we cannot reach offset from start, lookup the first field
     and start from there.  */
  if (start->offset > offset)
    start = get_varinfo (start->head);

  /* We may not find a variable in the field list with the actual
     offset when when we have glommed a structure to a variable.
     In that case, however, offset should still be within the size
     of the variable.
     If we got beyond the offset we look for return the field
     directly preceding offset which may be the last field.  */
  while (start->next
	 && offset >= start->offset
	 && !((offset - start->offset) < start->size))
    start = vi_next (start);

  return start;
}


/* This structure is used during pushing fields onto the fieldstack
   to track the offset of the field, since bitpos_of_field gives it
   relative to its immediate containing type, and we want it relative
   to the ultimate containing object.  */

struct fieldoff
{
  /* Offset from the base of the base containing object to this field.  */
  HOST_WIDE_INT offset;

  /* Size, in bits, of the field.  */
  unsigned HOST_WIDE_INT size;

  unsigned has_unknown_size : 1;

  unsigned must_have_pointers : 1;

  unsigned may_have_pointers : 1;

  unsigned only_restrict_pointers : 1;
};
typedef struct fieldoff fieldoff_s;


/* qsort comparison function for two fieldoff's PA and PB */

static int
fieldoff_compare (const void *pa, const void *pb)
{
  const fieldoff_s *foa = (const fieldoff_s *)pa;
  const fieldoff_s *fob = (const fieldoff_s *)pb;
  unsigned HOST_WIDE_INT foasize, fobsize;

  if (foa->offset < fob->offset)
    return -1;
  else if (foa->offset > fob->offset)
    return 1;

  foasize = foa->size;
  fobsize = fob->size;
  if (foasize < fobsize)
    return -1;
  else if (foasize > fobsize)
    return 1;
  return 0;
}

/* Sort a fieldstack according to the field offset and sizes.  */
static void
sort_fieldstack (vec<fieldoff_s> fieldstack)
{
  fieldstack.qsort (fieldoff_compare);
}

/* Return true if T is a type that can have subvars.  */

static inline bool
type_can_have_subvars (const_tree t)
{
  /* Aggregates without overlapping fields can have subvars.  */
  return TREE_CODE (t) == RECORD_TYPE;
}

/* Return true if V is a tree that we can have subvars for.
   Normally, this is any aggregate type.  Also complex
   types which are not gimple registers can have subvars.  */

static inline bool
var_can_have_subvars (const_tree v)
{
  /* Volatile variables should never have subvars.  */
  if (TREE_THIS_VOLATILE (v))
    return false;

  /* Non decls or memory tags can never have subvars.  */
  if (!DECL_P (v))
    return false;

  return type_can_have_subvars (TREE_TYPE (v));
}

/* Return true if T is a type that does contain pointers.  */

static bool
type_must_have_pointers (tree type)
{
  if (POINTER_TYPE_P (type))
    return true;

  if (TREE_CODE (type) == ARRAY_TYPE)
    return type_must_have_pointers (TREE_TYPE (type));

  /* A function or method can have pointers as arguments, so track
     those separately.  */
  if (TREE_CODE (type) == FUNCTION_TYPE
      || TREE_CODE (type) == METHOD_TYPE)
    return true;

  return false;
}

static bool
field_must_have_pointers (tree t)
{
  return type_must_have_pointers (TREE_TYPE (t));
}

/* Given a TYPE, and a vector of field offsets FIELDSTACK, push all
   the fields of TYPE onto fieldstack, recording their offsets along
   the way.

   OFFSET is used to keep track of the offset in this entire
   structure, rather than just the immediately containing structure.
   Returns false if the caller is supposed to handle the field we
   recursed for.  */

static bool
push_fields_onto_fieldstack (tree type, vec<fieldoff_s> *fieldstack,
			     HOST_WIDE_INT offset)
{
  tree field;
  bool empty_p = true;

  if (TREE_CODE (type) != RECORD_TYPE)
    return false;

  /* If the vector of fields is growing too big, bail out early.
     Callers check for vec::length <= MAX_FIELDS_FOR_FIELD_SENSITIVE, make
     sure this fails.  */
  if (fieldstack->length () > MAX_FIELDS_FOR_FIELD_SENSITIVE)
    return false;

  for (field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
    if (TREE_CODE (field) == FIELD_DECL)
      {
	bool push = false;
	HOST_WIDE_INT foff = bitpos_of_field (field);

	if (!var_can_have_subvars (field)
	    || TREE_CODE (TREE_TYPE (field)) == QUAL_UNION_TYPE
	    || TREE_CODE (TREE_TYPE (field)) == UNION_TYPE)
	  push = true;
	else if (!push_fields_onto_fieldstack
		    (TREE_TYPE (field), fieldstack, offset + foff)
		 && (DECL_SIZE (field)
		     && !integer_zerop (DECL_SIZE (field))))
	  /* Empty structures may have actual size, like in C++.  So
	     see if we didn't push any subfields and the size is
	     nonzero, push the field onto the stack.  */
	  push = true;

	if (push)
	  {
	    fieldoff_s *pair = NULL;
	    bool has_unknown_size = false;
	    bool must_have_pointers_p;

	    if (!fieldstack->is_empty ())
	      pair = &fieldstack->last ();

	    /* If there isn't anything at offset zero, create sth.  */
	    if (!pair
		&& offset + foff != 0)
	      {
		fieldoff_s e = {0, offset + foff, false, false, false, false};
		pair = fieldstack->safe_push (e);
	      }

	    if (!DECL_SIZE (field)
		|| !host_integerp (DECL_SIZE (field), 1))
	      has_unknown_size = true;

	    /* If adjacent fields do not contain pointers merge them.  */
	    must_have_pointers_p = field_must_have_pointers (field);
	    if (pair
		&& !has_unknown_size
		&& !must_have_pointers_p
		&& !pair->must_have_pointers
		&& !pair->has_unknown_size
		&& pair->offset + (HOST_WIDE_INT)pair->size == offset + foff)
	      {
		pair->size += TREE_INT_CST_LOW (DECL_SIZE (field));
	      }
	    else
	      {
		fieldoff_s e;
		e.offset = offset + foff;
		e.has_unknown_size = has_unknown_size;
		if (!has_unknown_size)
		  e.size = TREE_INT_CST_LOW (DECL_SIZE (field));
		else
		  e.size = -1;
		e.must_have_pointers = must_have_pointers_p;
		e.may_have_pointers = true;
		e.only_restrict_pointers
		  = (!has_unknown_size
		     && POINTER_TYPE_P (TREE_TYPE (field))
		     && TYPE_RESTRICT (TREE_TYPE (field)));
		fieldstack->safe_push (e);
	      }
	  }

	empty_p = false;
      }

  return !empty_p;
}

/* Count the number of arguments DECL has, and set IS_VARARGS to true
   if it is a varargs function.  */

static unsigned int
count_num_arguments (tree decl, bool *is_varargs)
{
  unsigned int num = 0;
  tree t;

  /* Capture named arguments for K&R functions.  They do not
     have a prototype and thus no TYPE_ARG_TYPES.  */
  for (t = DECL_ARGUMENTS (decl); t; t = DECL_CHAIN (t))
    ++num;

  /* Check if the function has variadic arguments.  */
  for (t = TYPE_ARG_TYPES (TREE_TYPE (decl)); t; t = TREE_CHAIN (t))
    if (TREE_VALUE (t) == void_type_node)
      break;
  if (!t)
    *is_varargs = true;

  return num;
}

/* Creation function node for DECL, using NAME, and return the index
   of the variable we've created for the function.  */

static varinfo_t
create_function_info_for (tree decl, const char *name)
{
  struct function *fn = DECL_STRUCT_FUNCTION (decl);
  varinfo_t vi, prev_vi;
  tree arg;
  unsigned int i;
  bool is_varargs = false;
  unsigned int num_args = count_num_arguments (decl, &is_varargs);

  /* Create the variable info.  */

  vi = new_var_info (decl, name);
  vi->offset = 0;
  vi->size = 1;
  vi->fullsize = fi_parm_base + num_args;
  vi->is_fn_info = 1;
  vi->may_have_pointers = false;
  if (is_varargs)
    vi->fullsize = ~0;
  insert_vi_for_tree (vi->decl, vi);

  prev_vi = vi;

  /* Create a variable for things the function clobbers and one for
     things the function uses.  */
    {
      varinfo_t clobbervi, usevi;
      const char *newname;
      char *tempname;

      asprintf (&tempname, "%s.clobber", name);
      newname = ggc_strdup (tempname);
      free (tempname);

      clobbervi = new_var_info (NULL, newname);
      clobbervi->offset = fi_clobbers;
      clobbervi->size = 1;
      clobbervi->fullsize = vi->fullsize;
      clobbervi->is_full_var = true;
      clobbervi->is_global_var = false;
      gcc_assert (prev_vi->offset < clobbervi->offset);
      prev_vi->next = clobbervi->id;
      prev_vi = clobbervi;

      asprintf (&tempname, "%s.use", name);
      newname = ggc_strdup (tempname);
      free (tempname);

      usevi = new_var_info (NULL, newname);
      usevi->offset = fi_uses;
      usevi->size = 1;
      usevi->fullsize = vi->fullsize;
      usevi->is_full_var = true;
      usevi->is_global_var = false;
      gcc_assert (prev_vi->offset < usevi->offset);
      prev_vi->next = usevi->id;
      prev_vi = usevi;
    }

  /* And one for the static chain.  */
  if (fn->static_chain_decl != NULL_TREE)
    {
      varinfo_t chainvi;
      const char *newname;
      char *tempname;

      asprintf (&tempname, "%s.chain", name);
      newname = ggc_strdup (tempname);
      free (tempname);

      chainvi = new_var_info (fn->static_chain_decl, newname);
      chainvi->offset = fi_static_chain;
      chainvi->size = 1;
      chainvi->fullsize = vi->fullsize;
      chainvi->is_full_var = true;
      chainvi->is_global_var = false;
      gcc_assert (prev_vi->offset < chainvi->offset);
      prev_vi->next = chainvi->id;
      prev_vi = chainvi;
      insert_vi_for_tree (fn->static_chain_decl, chainvi);
    }

  /* Create a variable for the return var.  */
  if (DECL_RESULT (decl) != NULL
      || !VOID_TYPE_P (TREE_TYPE (TREE_TYPE (decl))))
    {
      varinfo_t resultvi;
      const char *newname;
      char *tempname;
      tree resultdecl = decl;

      if (DECL_RESULT (decl))
	resultdecl = DECL_RESULT (decl);

      asprintf (&tempname, "%s.result", name);
      newname = ggc_strdup (tempname);
      free (tempname);

      resultvi = new_var_info (resultdecl, newname);
      resultvi->offset = fi_result;
      resultvi->size = 1;
      resultvi->fullsize = vi->fullsize;
      resultvi->is_full_var = true;
      if (DECL_RESULT (decl))
	resultvi->may_have_pointers = true;
      gcc_assert (prev_vi->offset < resultvi->offset);
      prev_vi->next = resultvi->id;
      prev_vi = resultvi;
      if (DECL_RESULT (decl))
	insert_vi_for_tree (DECL_RESULT (decl), resultvi);
    }

  /* Set up variables for each argument.  */
  arg = DECL_ARGUMENTS (decl);
  for (i = 0; i < num_args; i++)
    {
      varinfo_t argvi;
      const char *newname;
      char *tempname;
      tree argdecl = decl;

      if (arg)
	argdecl = arg;

      asprintf (&tempname, "%s.arg%d", name, i);
      newname = ggc_strdup (tempname);
      free (tempname);

      argvi = new_var_info (argdecl, newname);
      argvi->offset = fi_parm_base + i;
      argvi->size = 1;
      argvi->is_full_var = true;
      argvi->fullsize = vi->fullsize;
      if (arg)
	argvi->may_have_pointers = true;
      gcc_assert (prev_vi->offset < argvi->offset);
      prev_vi->next = argvi->id;
      prev_vi = argvi;
      if (arg)
	{
	  insert_vi_for_tree (arg, argvi);
	  arg = DECL_CHAIN (arg);
	}
    }

  /* Add one representative for all further args.  */
  if (is_varargs)
    {
      varinfo_t argvi;
      const char *newname;
      char *tempname;
      tree decl;

      asprintf (&tempname, "%s.varargs", name);
      newname = ggc_strdup (tempname);
      free (tempname);

      /* We need sth that can be pointed to for va_start.  */
      decl = build_fake_var_decl (ptr_type_node);

      argvi = new_var_info (decl, newname);
      argvi->offset = fi_parm_base + num_args;
      argvi->size = ~0;
      argvi->is_full_var = true;
      argvi->is_heap_var = true;
      argvi->fullsize = vi->fullsize;
      gcc_assert (prev_vi->offset < argvi->offset);
      prev_vi->next = argvi->id;
      prev_vi = argvi;
    }

  return vi;
}


/* Return true if FIELDSTACK contains fields that overlap.
   FIELDSTACK is assumed to be sorted by offset.  */

static bool
check_for_overlaps (vec<fieldoff_s> fieldstack)
{
  fieldoff_s *fo = NULL;
  unsigned int i;
  HOST_WIDE_INT lastoffset = -1;

  FOR_EACH_VEC_ELT (fieldstack, i, fo)
    {
      if (fo->offset == lastoffset)
	return true;
      lastoffset = fo->offset;
    }
  return false;
}

/* Create a varinfo structure for NAME and DECL, and add it to VARMAP.
   This will also create any varinfo structures necessary for fields
   of DECL.  */

static varinfo_t
create_variable_info_for_1 (tree decl, const char *name)
{
  varinfo_t vi, newvi;
  tree decl_type = TREE_TYPE (decl);
  tree declsize = DECL_P (decl) ? DECL_SIZE (decl) : TYPE_SIZE (decl_type);
  vec<fieldoff_s> fieldstack = vNULL;
  fieldoff_s *fo;
  unsigned int i;

  if (!declsize
      || !host_integerp (declsize, 1))
    {
      vi = new_var_info (decl, name);
      vi->offset = 0;
      vi->size = ~0;
      vi->fullsize = ~0;
      vi->is_unknown_size_var = true;
      vi->is_full_var = true;
      vi->may_have_pointers = true;
      return vi;
    }

  /* Collect field information.  */
  if (use_field_sensitive
      && var_can_have_subvars (decl)
      /* ???  Force us to not use subfields for global initializers
	 in IPA mode.  Else we'd have to parse arbitrary initializers.  */
      && !(in_ipa_mode
	   && is_global_var (decl)
	   && DECL_INITIAL (decl)))
    {
      fieldoff_s *fo = NULL;
      bool notokay = false;
      unsigned int i;

      push_fields_onto_fieldstack (decl_type, &fieldstack, 0);

      for (i = 0; !notokay && fieldstack.iterate (i, &fo); i++)
	if (fo->has_unknown_size
	    || fo->offset < 0)
	  {
	    notokay = true;
	    break;
	  }

      /* We can't sort them if we have a field with a variable sized type,
	 which will make notokay = true.  In that case, we are going to return
	 without creating varinfos for the fields anyway, so sorting them is a
	 waste to boot.  */
      if (!notokay)
	{
	  sort_fieldstack (fieldstack);
	  /* Due to some C++ FE issues, like PR 22488, we might end up
	     what appear to be overlapping fields even though they,
	     in reality, do not overlap.  Until the C++ FE is fixed,
	     we will simply disable field-sensitivity for these cases.  */
	  notokay = check_for_overlaps (fieldstack);
	}

      if (notokay)
	fieldstack.release ();
    }

  /* If we didn't end up collecting sub-variables create a full
     variable for the decl.  */
  if (fieldstack.length () <= 1
      || fieldstack.length () > MAX_FIELDS_FOR_FIELD_SENSITIVE)
    {
      vi = new_var_info (decl, name);
      vi->offset = 0;
      vi->may_have_pointers = true;
      vi->fullsize = TREE_INT_CST_LOW (declsize);
      vi->size = vi->fullsize;
      vi->is_full_var = true;
      fieldstack.release ();
      return vi;
    }

  vi = new_var_info (decl, name);
  vi->fullsize = TREE_INT_CST_LOW (declsize);
  for (i = 0, newvi = vi;
       fieldstack.iterate (i, &fo);
       ++i, newvi = vi_next (newvi))
    {
      const char *newname = "NULL";
      char *tempname;

      if (dump_file)
	{
	  asprintf (&tempname, "%s." HOST_WIDE_INT_PRINT_DEC
		    "+" HOST_WIDE_INT_PRINT_DEC, name, fo->offset, fo->size);
	  newname = ggc_strdup (tempname);
	  free (tempname);
	}
      newvi->name = newname;
      newvi->offset = fo->offset;
      newvi->size = fo->size;
      newvi->fullsize = vi->fullsize;
      newvi->may_have_pointers = fo->may_have_pointers;
      newvi->only_restrict_pointers = fo->only_restrict_pointers;
      if (i + 1 < fieldstack.length ())
	{
	  varinfo_t tem = new_var_info (decl, name);
	  newvi->next = tem->id;
	  tem->head = vi->id;
	}
    }

  fieldstack.release ();

  return vi;
}

static unsigned int
create_variable_info_for (tree decl, const char *name)
{
  varinfo_t vi = create_variable_info_for_1 (decl, name);
  unsigned int id = vi->id;

  insert_vi_for_tree (decl, vi);

  if (TREE_CODE (decl) != VAR_DECL)
    return id;

  /* Create initial constraints for globals.  */
  for (; vi; vi = vi_next (vi))
    {
      if (!vi->may_have_pointers
	  || !vi->is_global_var)
	continue;

      /* Mark global restrict qualified pointers.  */
      if ((POINTER_TYPE_P (TREE_TYPE (decl))
	   && TYPE_RESTRICT (TREE_TYPE (decl)))
	  || vi->only_restrict_pointers)
	{
	  make_constraint_from_global_restrict (vi, "GLOBAL_RESTRICT");
	  continue;
	}

      /* In non-IPA mode the initializer from nonlocal is all we need.  */
      if (!in_ipa_mode
	  || DECL_HARD_REGISTER (decl))
	make_copy_constraint (vi, nonlocal_id);

      /* In IPA mode parse the initializer and generate proper constraints
	 for it.  */
      else
	{
	  struct varpool_node *vnode = varpool_get_node (decl);

	  /* For escaped variables initialize them from nonlocal.  */
	  if (!varpool_all_refs_explicit_p (vnode))
	    make_copy_constraint (vi, nonlocal_id);

	  /* If this is a global variable with an initializer and we are in
	     IPA mode generate constraints for it.  */
	  if (DECL_INITIAL (decl)
	      && vnode->symbol.definition)
	    {
	      vec<ce_s> rhsc = vNULL;
	      struct constraint_expr lhs, *rhsp;
	      unsigned i;
	      get_constraint_for_rhs (DECL_INITIAL (decl), &rhsc);
	      lhs.var = vi->id;
	      lhs.offset = 0;
	      lhs.type = SCALAR;
	      FOR_EACH_VEC_ELT (rhsc, i, rhsp)
		process_constraint (new_constraint (lhs, *rhsp));
	      /* If this is a variable that escapes from the unit
		 the initializer escapes as well.  */
	      if (!varpool_all_refs_explicit_p (vnode))
		{
		  lhs.var = escaped_id;
		  lhs.offset = 0;
		  lhs.type = SCALAR;
		  FOR_EACH_VEC_ELT (rhsc, i, rhsp)
		    process_constraint (new_constraint (lhs, *rhsp));
		}
	      rhsc.release ();
	    }
	}
    }

  return id;
}

/* Print out the points-to solution for VAR to FILE.  */

static void
dump_solution_for_var (FILE *file, unsigned int var)
{
  varinfo_t vi = get_varinfo (var);
  unsigned int i;
  bitmap_iterator bi;

  /* Dump the solution for unified vars anyway, this avoids difficulties
     in scanning dumps in the testsuite.  */
  fprintf (file, "%s = { ", vi->name);
  vi = get_varinfo (find (var));
  EXECUTE_IF_SET_IN_BITMAP (vi->solution, 0, i, bi)
    fprintf (file, "%s ", get_varinfo (i)->name);
  fprintf (file, "}");

  /* But note when the variable was unified.  */
  if (vi->id != var)
    fprintf (file, " same as %s", vi->name);

  fprintf (file, "\n");
}

/* Print the points-to solution for VAR to stdout.  */

DEBUG_FUNCTION void
debug_solution_for_var (unsigned int var)
{
  dump_solution_for_var (stdout, var);
}

/* Create varinfo structures for all of the variables in the
   function for intraprocedural mode.  */

static void
intra_create_variable_infos (void)
{
  tree t;

  /* For each incoming pointer argument arg, create the constraint ARG
     = NONLOCAL or a dummy variable if it is a restrict qualified
     passed-by-reference argument.  */
  for (t = DECL_ARGUMENTS (current_function_decl); t; t = DECL_CHAIN (t))
    {
      varinfo_t p = get_vi_for_tree (t);

      /* For restrict qualified pointers to objects passed by
         reference build a real representative for the pointed-to object.
	 Treat restrict qualified references the same.  */
      if (TYPE_RESTRICT (TREE_TYPE (t))
	  && ((DECL_BY_REFERENCE (t) && POINTER_TYPE_P (TREE_TYPE (t)))
	      || TREE_CODE (TREE_TYPE (t)) == REFERENCE_TYPE)
	  && !type_contains_placeholder_p (TREE_TYPE (TREE_TYPE (t))))
	{
	  struct constraint_expr lhsc, rhsc;
	  varinfo_t vi;
	  tree heapvar = build_fake_var_decl (TREE_TYPE (TREE_TYPE (t)));
	  DECL_EXTERNAL (heapvar) = 1;
	  vi = create_variable_info_for_1 (heapvar, "PARM_NOALIAS");
	  insert_vi_for_tree (heapvar, vi);
	  lhsc.var = p->id;
	  lhsc.type = SCALAR;
	  lhsc.offset = 0;
	  rhsc.var = vi->id;
	  rhsc.type = ADDRESSOF;
	  rhsc.offset = 0;
	  process_constraint (new_constraint (lhsc, rhsc));
	  for (; vi; vi = vi_next (vi))
	    if (vi->may_have_pointers)
	      {
		if (vi->only_restrict_pointers)
		  make_constraint_from_global_restrict (vi, "GLOBAL_RESTRICT");
		else
		  make_copy_constraint (vi, nonlocal_id);
	      }
	  continue;
	}

      if (POINTER_TYPE_P (TREE_TYPE (t))
	  && TYPE_RESTRICT (TREE_TYPE (t)))
	make_constraint_from_global_restrict (p, "PARM_RESTRICT");
      else
	{
	  for (; p; p = vi_next (p))
	    {
	      if (p->only_restrict_pointers)
		make_constraint_from_global_restrict (p, "PARM_RESTRICT");
	      else if (p->may_have_pointers)
		make_constraint_from (p, nonlocal_id);
	    }
	}
    }

  /* Add a constraint for a result decl that is passed by reference.  */
  if (DECL_RESULT (cfun->decl)
      && DECL_BY_REFERENCE (DECL_RESULT (cfun->decl)))
    {
      varinfo_t p, result_vi = get_vi_for_tree (DECL_RESULT (cfun->decl));

      for (p = result_vi; p; p = vi_next (p))
	make_constraint_from (p, nonlocal_id);
    }

  /* Add a constraint for the incoming static chain parameter.  */
  if (cfun->static_chain_decl != NULL_TREE)
    {
      varinfo_t p, chain_vi = get_vi_for_tree (cfun->static_chain_decl);

      for (p = chain_vi; p; p = vi_next (p))
	make_constraint_from (p, nonlocal_id);
    }
}

/* Structure used to put solution bitmaps in a hashtable so they can
   be shared among variables with the same points-to set.  */

typedef struct shared_bitmap_info
{
  bitmap pt_vars;
  hashval_t hashcode;
} *shared_bitmap_info_t;
typedef const struct shared_bitmap_info *const_shared_bitmap_info_t;

/* Shared_bitmap hashtable helpers.  */

struct shared_bitmap_hasher : typed_free_remove <shared_bitmap_info>
{
  typedef shared_bitmap_info value_type;
  typedef shared_bitmap_info compare_type;
  static inline hashval_t hash (const value_type *);
  static inline bool equal (const value_type *, const compare_type *);
};

/* Hash function for a shared_bitmap_info_t */

inline hashval_t
shared_bitmap_hasher::hash (const value_type *bi)
{
  return bi->hashcode;
}

/* Equality function for two shared_bitmap_info_t's. */

inline bool
shared_bitmap_hasher::equal (const value_type *sbi1, const compare_type *sbi2)
{
  return bitmap_equal_p (sbi1->pt_vars, sbi2->pt_vars);
}

/* Shared_bitmap hashtable.  */

static hash_table <shared_bitmap_hasher> shared_bitmap_table;

/* Lookup a bitmap in the shared bitmap hashtable, and return an already
   existing instance if there is one, NULL otherwise.  */

static bitmap
shared_bitmap_lookup (bitmap pt_vars)
{
  shared_bitmap_info **slot;
  struct shared_bitmap_info sbi;

  sbi.pt_vars = pt_vars;
  sbi.hashcode = bitmap_hash (pt_vars);

  slot = shared_bitmap_table.find_slot_with_hash (&sbi, sbi.hashcode,
						  NO_INSERT);
  if (!slot)
    return NULL;
  else
    return (*slot)->pt_vars;
}


/* Add a bitmap to the shared bitmap hashtable.  */

static void
shared_bitmap_add (bitmap pt_vars)
{
  shared_bitmap_info **slot;
  shared_bitmap_info_t sbi = XNEW (struct shared_bitmap_info);

  sbi->pt_vars = pt_vars;
  sbi->hashcode = bitmap_hash (pt_vars);

  slot = shared_bitmap_table.find_slot_with_hash (sbi, sbi->hashcode, INSERT);
  gcc_assert (!*slot);
  *slot = sbi;
}


/* Set bits in INTO corresponding to the variable uids in solution set FROM.  */

static void
set_uids_in_ptset (bitmap into, bitmap from, struct pt_solution *pt)
{
  unsigned int i;
  bitmap_iterator bi;

  EXECUTE_IF_SET_IN_BITMAP (from, 0, i, bi)
    {
      varinfo_t vi = get_varinfo (i);

      /* The only artificial variables that are allowed in a may-alias
	 set are heap variables.  */
      if (vi->is_artificial_var && !vi->is_heap_var)
	continue;

      if (TREE_CODE (vi->decl) == VAR_DECL
	  || TREE_CODE (vi->decl) == PARM_DECL
	  || TREE_CODE (vi->decl) == RESULT_DECL)
	{
	  /* If we are in IPA mode we will not recompute points-to
	     sets after inlining so make sure they stay valid.  */
	  if (in_ipa_mode
	      && !DECL_PT_UID_SET_P (vi->decl))
	    SET_DECL_PT_UID (vi->decl, DECL_UID (vi->decl));

	  /* Add the decl to the points-to set.  Note that the points-to
	     set contains global variables.  */
	  bitmap_set_bit (into, DECL_PT_UID (vi->decl));
	  if (vi->is_global_var)
	    pt->vars_contains_global = true;
	}
    }
}


/* Compute the points-to solution *PT for the variable VI.  */

static struct pt_solution
find_what_var_points_to (varinfo_t orig_vi)
{
  unsigned int i;
  bitmap_iterator bi;
  bitmap finished_solution;
  bitmap result;
  varinfo_t vi;
  void **slot;
  struct pt_solution *pt;

  /* This variable may have been collapsed, let's get the real
     variable.  */
  vi = get_varinfo (find (orig_vi->id));

  /* See if we have already computed the solution and return it.  */
  slot = pointer_map_insert (final_solutions, vi);
  if (*slot != NULL)
    return *(struct pt_solution *)*slot;

  *slot = pt = XOBNEW (&final_solutions_obstack, struct pt_solution);
  memset (pt, 0, sizeof (struct pt_solution));

  /* Translate artificial variables into SSA_NAME_PTR_INFO
     attributes.  */
  EXECUTE_IF_SET_IN_BITMAP (vi->solution, 0, i, bi)
    {
      varinfo_t vi = get_varinfo (i);

      if (vi->is_artificial_var)
	{
	  if (vi->id == nothing_id)
	    pt->null = 1;
	  else if (vi->id == escaped_id)
	    {
	      if (in_ipa_mode)
		pt->ipa_escaped = 1;
	      else
		pt->escaped = 1;
	    }
	  else if (vi->id == nonlocal_id)
	    pt->nonlocal = 1;
	  else if (vi->is_heap_var)
	    /* We represent heapvars in the points-to set properly.  */
	    ;
	  else if (vi->id == readonly_id)
	    /* Nobody cares.  */
	    ;
	  else if (vi->id == anything_id
		   || vi->id == integer_id)
	    pt->anything = 1;
	}
    }

  /* Instead of doing extra work, simply do not create
     elaborate points-to information for pt_anything pointers.  */
  if (pt->anything)
    return *pt;

  /* Share the final set of variables when possible.  */
  finished_solution = BITMAP_GGC_ALLOC ();
  stats.points_to_sets_created++;

  set_uids_in_ptset (finished_solution, vi->solution, pt);
  result = shared_bitmap_lookup (finished_solution);
  if (!result)
    {
      shared_bitmap_add (finished_solution);
      pt->vars = finished_solution;
    }
  else
    {
      pt->vars = result;
      bitmap_clear (finished_solution);
    }

  return *pt;
}

/* Given a pointer variable P, fill in its points-to set.  */

static void
find_what_p_points_to (tree p)
{
  struct ptr_info_def *pi;
  tree lookup_p = p;
  varinfo_t vi;

  /* For parameters, get at the points-to set for the actual parm
     decl.  */
  if (TREE_CODE (p) == SSA_NAME
      && SSA_NAME_IS_DEFAULT_DEF (p)
      && (TREE_CODE (SSA_NAME_VAR (p)) == PARM_DECL
	  || TREE_CODE (SSA_NAME_VAR (p)) == RESULT_DECL))
    lookup_p = SSA_NAME_VAR (p);

  vi = lookup_vi_for_tree (lookup_p);
  if (!vi)
    return;

  pi = get_ptr_info (p);
  pi->pt = find_what_var_points_to (vi);
}


/* Query statistics for points-to solutions.  */

static struct {
  unsigned HOST_WIDE_INT pt_solution_includes_may_alias;
  unsigned HOST_WIDE_INT pt_solution_includes_no_alias;
  unsigned HOST_WIDE_INT pt_solutions_intersect_may_alias;
  unsigned HOST_WIDE_INT pt_solutions_intersect_no_alias;
} pta_stats;

void
dump_pta_stats (FILE *s)
{
  fprintf (s, "\nPTA query stats:\n");
  fprintf (s, "  pt_solution_includes: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   pta_stats.pt_solution_includes_no_alias,
	   pta_stats.pt_solution_includes_no_alias
	   + pta_stats.pt_solution_includes_may_alias);
  fprintf (s, "  pt_solutions_intersect: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   pta_stats.pt_solutions_intersect_no_alias,
	   pta_stats.pt_solutions_intersect_no_alias
	   + pta_stats.pt_solutions_intersect_may_alias);
}


/* Reset the points-to solution *PT to a conservative default
   (point to anything).  */

void
pt_solution_reset (struct pt_solution *pt)
{
  memset (pt, 0, sizeof (struct pt_solution));
  pt->anything = true;
}

/* Set the points-to solution *PT to point only to the variables
   in VARS.  VARS_CONTAINS_GLOBAL specifies whether that contains
   global variables and VARS_CONTAINS_RESTRICT specifies whether
   it contains restrict tag variables.  */

void
pt_solution_set (struct pt_solution *pt, bitmap vars, bool vars_contains_global)
{
  memset (pt, 0, sizeof (struct pt_solution));
  pt->vars = vars;
  pt->vars_contains_global = vars_contains_global;
}

/* Set the points-to solution *PT to point only to the variable VAR.  */

void
pt_solution_set_var (struct pt_solution *pt, tree var)
{
  memset (pt, 0, sizeof (struct pt_solution));
  pt->vars = BITMAP_GGC_ALLOC ();
  bitmap_set_bit (pt->vars, DECL_PT_UID (var));
  pt->vars_contains_global = is_global_var (var);
}

/* Computes the union of the points-to solutions *DEST and *SRC and
   stores the result in *DEST.  This changes the points-to bitmap
   of *DEST and thus may not be used if that might be shared.
   The points-to bitmap of *SRC and *DEST will not be shared after
   this function if they were not before.  */

static void
pt_solution_ior_into (struct pt_solution *dest, struct pt_solution *src)
{
  dest->anything |= src->anything;
  if (dest->anything)
    {
      pt_solution_reset (dest);
      return;
    }

  dest->nonlocal |= src->nonlocal;
  dest->escaped |= src->escaped;
  dest->ipa_escaped |= src->ipa_escaped;
  dest->null |= src->null;
  dest->vars_contains_global |= src->vars_contains_global;
  if (!src->vars)
    return;

  if (!dest->vars)
    dest->vars = BITMAP_GGC_ALLOC ();
  bitmap_ior_into (dest->vars, src->vars);
}

/* Return true if the points-to solution *PT is empty.  */

bool
pt_solution_empty_p (struct pt_solution *pt)
{
  if (pt->anything
      || pt->nonlocal)
    return false;

  if (pt->vars
      && !bitmap_empty_p (pt->vars))
    return false;

  /* If the solution includes ESCAPED, check if that is empty.  */
  if (pt->escaped
      && !pt_solution_empty_p (&cfun->gimple_df->escaped))
    return false;

  /* If the solution includes ESCAPED, check if that is empty.  */
  if (pt->ipa_escaped
      && !pt_solution_empty_p (&ipa_escaped_pt))
    return false;

  return true;
}

/* Return true if the points-to solution *PT only point to a single var, and
   return the var uid in *UID.  */

bool
pt_solution_singleton_p (struct pt_solution *pt, unsigned *uid)
{
  if (pt->anything || pt->nonlocal || pt->escaped || pt->ipa_escaped
      || pt->null || pt->vars == NULL
      || !bitmap_single_bit_set_p (pt->vars))
    return false;

  *uid = bitmap_first_set_bit (pt->vars);
  return true;
}

/* Return true if the points-to solution *PT includes global memory.  */

bool
pt_solution_includes_global (struct pt_solution *pt)
{
  if (pt->anything
      || pt->nonlocal
      || pt->vars_contains_global)
    return true;

  if (pt->escaped)
    return pt_solution_includes_global (&cfun->gimple_df->escaped);

  if (pt->ipa_escaped)
    return pt_solution_includes_global (&ipa_escaped_pt);

  /* ???  This predicate is not correct for the IPA-PTA solution
     as we do not properly distinguish between unit escape points
     and global variables.  */
  if (cfun->gimple_df->ipa_pta)
    return true;

  return false;
}

/* Return true if the points-to solution *PT includes the variable
   declaration DECL.  */

static bool
pt_solution_includes_1 (struct pt_solution *pt, const_tree decl)
{
  if (pt->anything)
    return true;

  if (pt->nonlocal
      && is_global_var (decl))
    return true;

  if (pt->vars
      && bitmap_bit_p (pt->vars, DECL_PT_UID (decl)))
    return true;

  /* If the solution includes ESCAPED, check it.  */
  if (pt->escaped
      && pt_solution_includes_1 (&cfun->gimple_df->escaped, decl))
    return true;

  /* If the solution includes ESCAPED, check it.  */
  if (pt->ipa_escaped
      && pt_solution_includes_1 (&ipa_escaped_pt, decl))
    return true;

  return false;
}

bool
pt_solution_includes (struct pt_solution *pt, const_tree decl)
{
  bool res = pt_solution_includes_1 (pt, decl);
  if (res)
    ++pta_stats.pt_solution_includes_may_alias;
  else
    ++pta_stats.pt_solution_includes_no_alias;
  return res;
}

/* Return true if both points-to solutions PT1 and PT2 have a non-empty
   intersection.  */

static bool
pt_solutions_intersect_1 (struct pt_solution *pt1, struct pt_solution *pt2)
{
  if (pt1->anything || pt2->anything)
    return true;

  /* If either points to unknown global memory and the other points to
     any global memory they alias.  */
  if ((pt1->nonlocal
       && (pt2->nonlocal
	   || pt2->vars_contains_global))
      || (pt2->nonlocal
	  && pt1->vars_contains_global))
    return true;

  /* Check the escaped solution if required.  */
  if ((pt1->escaped || pt2->escaped)
      && !pt_solution_empty_p (&cfun->gimple_df->escaped))
    {
      /* If both point to escaped memory and that solution
	 is not empty they alias.  */
      if (pt1->escaped && pt2->escaped)
	return true;

      /* If either points to escaped memory see if the escaped solution
	 intersects with the other.  */
      if ((pt1->escaped
	   && pt_solutions_intersect_1 (&cfun->gimple_df->escaped, pt2))
	  || (pt2->escaped
	      && pt_solutions_intersect_1 (&cfun->gimple_df->escaped, pt1)))
	return true;
    }

  /* Check the escaped solution if required.
     ???  Do we need to check the local against the IPA escaped sets?  */
  if ((pt1->ipa_escaped || pt2->ipa_escaped)
      && !pt_solution_empty_p (&ipa_escaped_pt))
    {
      /* If both point to escaped memory and that solution
	 is not empty they alias.  */
      if (pt1->ipa_escaped && pt2->ipa_escaped)
	return true;

      /* If either points to escaped memory see if the escaped solution
	 intersects with the other.  */
      if ((pt1->ipa_escaped
	   && pt_solutions_intersect_1 (&ipa_escaped_pt, pt2))
	  || (pt2->ipa_escaped
	      && pt_solutions_intersect_1 (&ipa_escaped_pt, pt1)))
	return true;
    }

  /* Now both pointers alias if their points-to solution intersects.  */
  return (pt1->vars
	  && pt2->vars
	  && bitmap_intersect_p (pt1->vars, pt2->vars));
}

bool
pt_solutions_intersect (struct pt_solution *pt1, struct pt_solution *pt2)
{
  bool res = pt_solutions_intersect_1 (pt1, pt2);
  if (res)
    ++pta_stats.pt_solutions_intersect_may_alias;
  else
    ++pta_stats.pt_solutions_intersect_no_alias;
  return res;
}


/* Dump points-to information to OUTFILE.  */

static void
dump_sa_points_to_info (FILE *outfile)
{
  unsigned int i;

  fprintf (outfile, "\nPoints-to sets\n\n");

  if (dump_flags & TDF_STATS)
    {
      fprintf (outfile, "Stats:\n");
      fprintf (outfile, "Total vars:               %d\n", stats.total_vars);
      fprintf (outfile, "Non-pointer vars:          %d\n",
	       stats.nonpointer_vars);
      fprintf (outfile, "Statically unified vars:  %d\n",
	       stats.unified_vars_static);
      fprintf (outfile, "Dynamically unified vars: %d\n",
	       stats.unified_vars_dynamic);
      fprintf (outfile, "Iterations:               %d\n", stats.iterations);
      fprintf (outfile, "Number of edges:          %d\n", stats.num_edges);
      fprintf (outfile, "Number of implicit edges: %d\n",
	       stats.num_implicit_edges);
    }

  for (i = 1; i < varmap.length (); i++)
    {
      varinfo_t vi = get_varinfo (i);
      if (!vi->may_have_pointers)
	continue;
      dump_solution_for_var (outfile, i);
    }
}


/* Debug points-to information to stderr.  */

DEBUG_FUNCTION void
debug_sa_points_to_info (void)
{
  dump_sa_points_to_info (stderr);
}


/* Initialize the always-existing constraint variables for NULL
   ANYTHING, READONLY, and INTEGER */

static void
init_base_vars (void)
{
  struct constraint_expr lhs, rhs;
  varinfo_t var_anything;
  varinfo_t var_nothing;
  varinfo_t var_readonly;
  varinfo_t var_escaped;
  varinfo_t var_nonlocal;
  varinfo_t var_storedanything;
  varinfo_t var_integer;

  /* Variable ID zero is reserved and should be NULL.  */
  varmap.safe_push (NULL);

  /* Create the NULL variable, used to represent that a variable points
     to NULL.  */
  var_nothing = new_var_info (NULL_TREE, "NULL");
  gcc_assert (var_nothing->id == nothing_id);
  var_nothing->is_artificial_var = 1;
  var_nothing->offset = 0;
  var_nothing->size = ~0;
  var_nothing->fullsize = ~0;
  var_nothing->is_special_var = 1;
  var_nothing->may_have_pointers = 0;
  var_nothing->is_global_var = 0;

  /* Create the ANYTHING variable, used to represent that a variable
     points to some unknown piece of memory.  */
  var_anything = new_var_info (NULL_TREE, "ANYTHING");
  gcc_assert (var_anything->id == anything_id);
  var_anything->is_artificial_var = 1;
  var_anything->size = ~0;
  var_anything->offset = 0;
  var_anything->fullsize = ~0;
  var_anything->is_special_var = 1;

  /* Anything points to anything.  This makes deref constraints just
     work in the presence of linked list and other p = *p type loops,
     by saying that *ANYTHING = ANYTHING. */
  lhs.type = SCALAR;
  lhs.var = anything_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = anything_id;
  rhs.offset = 0;

  /* This specifically does not use process_constraint because
     process_constraint ignores all anything = anything constraints, since all
     but this one are redundant.  */
  constraints.safe_push (new_constraint (lhs, rhs));

  /* Create the READONLY variable, used to represent that a variable
     points to readonly memory.  */
  var_readonly = new_var_info (NULL_TREE, "READONLY");
  gcc_assert (var_readonly->id == readonly_id);
  var_readonly->is_artificial_var = 1;
  var_readonly->offset = 0;
  var_readonly->size = ~0;
  var_readonly->fullsize = ~0;
  var_readonly->is_special_var = 1;

  /* readonly memory points to anything, in order to make deref
     easier.  In reality, it points to anything the particular
     readonly variable can point to, but we don't track this
     separately. */
  lhs.type = SCALAR;
  lhs.var = readonly_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = readonly_id;  /* FIXME */
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));

  /* Create the ESCAPED variable, used to represent the set of escaped
     memory.  */
  var_escaped = new_var_info (NULL_TREE, "ESCAPED");
  gcc_assert (var_escaped->id == escaped_id);
  var_escaped->is_artificial_var = 1;
  var_escaped->offset = 0;
  var_escaped->size = ~0;
  var_escaped->fullsize = ~0;
  var_escaped->is_special_var = 0;

  /* Create the NONLOCAL variable, used to represent the set of nonlocal
     memory.  */
  var_nonlocal = new_var_info (NULL_TREE, "NONLOCAL");
  gcc_assert (var_nonlocal->id == nonlocal_id);
  var_nonlocal->is_artificial_var = 1;
  var_nonlocal->offset = 0;
  var_nonlocal->size = ~0;
  var_nonlocal->fullsize = ~0;
  var_nonlocal->is_special_var = 1;

  /* ESCAPED = *ESCAPED, because escaped is may-deref'd at calls, etc.  */
  lhs.type = SCALAR;
  lhs.var = escaped_id;
  lhs.offset = 0;
  rhs.type = DEREF;
  rhs.var = escaped_id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));

  /* ESCAPED = ESCAPED + UNKNOWN_OFFSET, because if a sub-field escapes the
     whole variable escapes.  */
  lhs.type = SCALAR;
  lhs.var = escaped_id;
  lhs.offset = 0;
  rhs.type = SCALAR;
  rhs.var = escaped_id;
  rhs.offset = UNKNOWN_OFFSET;
  process_constraint (new_constraint (lhs, rhs));

  /* *ESCAPED = NONLOCAL.  This is true because we have to assume
     everything pointed to by escaped points to what global memory can
     point to.  */
  lhs.type = DEREF;
  lhs.var = escaped_id;
  lhs.offset = 0;
  rhs.type = SCALAR;
  rhs.var = nonlocal_id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));

  /* NONLOCAL = &NONLOCAL, NONLOCAL = &ESCAPED.  This is true because
     global memory may point to global memory and escaped memory.  */
  lhs.type = SCALAR;
  lhs.var = nonlocal_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = nonlocal_id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));
  rhs.type = ADDRESSOF;
  rhs.var = escaped_id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));

  /* Create the STOREDANYTHING variable, used to represent the set of
     variables stored to *ANYTHING.  */
  var_storedanything = new_var_info (NULL_TREE, "STOREDANYTHING");
  gcc_assert (var_storedanything->id == storedanything_id);
  var_storedanything->is_artificial_var = 1;
  var_storedanything->offset = 0;
  var_storedanything->size = ~0;
  var_storedanything->fullsize = ~0;
  var_storedanything->is_special_var = 0;

  /* Create the INTEGER variable, used to represent that a variable points
     to what an INTEGER "points to".  */
  var_integer = new_var_info (NULL_TREE, "INTEGER");
  gcc_assert (var_integer->id == integer_id);
  var_integer->is_artificial_var = 1;
  var_integer->size = ~0;
  var_integer->fullsize = ~0;
  var_integer->offset = 0;
  var_integer->is_special_var = 1;

  /* INTEGER = ANYTHING, because we don't know where a dereference of
     a random integer will point to.  */
  lhs.type = SCALAR;
  lhs.var = integer_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = anything_id;
  rhs.offset = 0;
  process_constraint (new_constraint (lhs, rhs));
}

/* Initialize things necessary to perform PTA */

static void
init_alias_vars (void)
{
  use_field_sensitive = (MAX_FIELDS_FOR_FIELD_SENSITIVE > 1);

  bitmap_obstack_initialize (&pta_obstack);
  bitmap_obstack_initialize (&oldpta_obstack);
  bitmap_obstack_initialize (&predbitmap_obstack);

  constraint_pool = create_alloc_pool ("Constraint pool",
				       sizeof (struct constraint), 30);
  variable_info_pool = create_alloc_pool ("Variable info pool",
					  sizeof (struct variable_info), 30);
  constraints.create (8);
  varmap.create (8);
  vi_for_tree = pointer_map_create ();
  call_stmt_vars = pointer_map_create ();

  memset (&stats, 0, sizeof (stats));
  shared_bitmap_table.create (511);
  init_base_vars ();

  gcc_obstack_init (&fake_var_decl_obstack);

  final_solutions = pointer_map_create ();
  gcc_obstack_init (&final_solutions_obstack);
}

/* Remove the REF and ADDRESS edges from GRAPH, as well as all the
   predecessor edges.  */

static void
remove_preds_and_fake_succs (constraint_graph_t graph)
{
  unsigned int i;

  /* Clear the implicit ref and address nodes from the successor
     lists.  */
  for (i = 1; i < FIRST_REF_NODE; i++)
    {
      if (graph->succs[i])
	bitmap_clear_range (graph->succs[i], FIRST_REF_NODE,
			    FIRST_REF_NODE * 2);
    }

  /* Free the successor list for the non-ref nodes.  */
  for (i = FIRST_REF_NODE + 1; i < graph->size; i++)
    {
      if (graph->succs[i])
	BITMAP_FREE (graph->succs[i]);
    }

  /* Now reallocate the size of the successor list as, and blow away
     the predecessor bitmaps.  */
  graph->size = varmap.length ();
  graph->succs = XRESIZEVEC (bitmap, graph->succs, graph->size);

  free (graph->implicit_preds);
  graph->implicit_preds = NULL;
  free (graph->preds);
  graph->preds = NULL;
  bitmap_obstack_release (&predbitmap_obstack);
}

/* Solve the constraint set.  */

static void
solve_constraints (void)
{
  struct scc_info *si;

  if (dump_file)
    fprintf (dump_file,
	     "\nCollapsing static cycles and doing variable "
	     "substitution\n");

  init_graph (varmap.length () * 2);

  if (dump_file)
    fprintf (dump_file, "Building predecessor graph\n");
  build_pred_graph ();

  if (dump_file)
    fprintf (dump_file, "Detecting pointer and location "
	     "equivalences\n");
  si = perform_var_substitution (graph);

  if (dump_file)
    fprintf (dump_file, "Rewriting constraints and unifying "
	     "variables\n");
  rewrite_constraints (graph, si);

  build_succ_graph ();

  free_var_substitution_info (si);

  /* Attach complex constraints to graph nodes.  */
  move_complex_constraints (graph);

  if (dump_file)
    fprintf (dump_file, "Uniting pointer but not location equivalent "
	     "variables\n");
  unite_pointer_equivalences (graph);

  if (dump_file)
    fprintf (dump_file, "Finding indirect cycles\n");
  find_indirect_cycles (graph);

  /* Implicit nodes and predecessors are no longer necessary at this
     point. */
  remove_preds_and_fake_succs (graph);

  if (dump_file && (dump_flags & TDF_GRAPH))
    {
      fprintf (dump_file, "\n\n// The constraint graph before solve-graph "
	       "in dot format:\n");
      dump_constraint_graph (dump_file);
      fprintf (dump_file, "\n\n");
    }

  if (dump_file)
    fprintf (dump_file, "Solving graph\n");

  solve_graph (graph);

  if (dump_file && (dump_flags & TDF_GRAPH))
    {
      fprintf (dump_file, "\n\n// The constraint graph after solve-graph "
	       "in dot format:\n");
      dump_constraint_graph (dump_file);
      fprintf (dump_file, "\n\n");
    }

  if (dump_file)
    dump_sa_points_to_info (dump_file);
}

/* Create points-to sets for the current function.  See the comments
   at the start of the file for an algorithmic overview.  */

static void
compute_points_to_sets (void)
{
  basic_block bb;
  unsigned i;
  varinfo_t vi;

  timevar_push (TV_TREE_PTA);

  init_alias_vars ();

  intra_create_variable_infos ();

  /* Now walk all statements and build the constraint set.  */
  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple phi = gsi_stmt (gsi);

	  if (! virtual_operand_p (gimple_phi_result (phi)))
	    find_func_aliases (phi);
	}

      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);

	  find_func_aliases (stmt);
	}
    }

  if (dump_file)
    {
      fprintf (dump_file, "Points-to analysis\n\nConstraints:\n\n");
      dump_constraints (dump_file, 0);
    }

  /* From the constraints compute the points-to sets.  */
  solve_constraints ();

  /* Compute the points-to set for ESCAPED used for call-clobber analysis.  */
  cfun->gimple_df->escaped = find_what_var_points_to (get_varinfo (escaped_id));

  /* Make sure the ESCAPED solution (which is used as placeholder in
     other solutions) does not reference itself.  This simplifies
     points-to solution queries.  */
  cfun->gimple_df->escaped.escaped = 0;

  /* Mark escaped HEAP variables as global.  */
  FOR_EACH_VEC_ELT (varmap, i, vi)
    if (vi
	&& vi->is_heap_var
	&& !vi->is_global_var)
      DECL_EXTERNAL (vi->decl) = vi->is_global_var
	= pt_solution_includes (&cfun->gimple_df->escaped, vi->decl);

  /* Compute the points-to sets for pointer SSA_NAMEs.  */
  for (i = 0; i < num_ssa_names; ++i)
    {
      tree ptr = ssa_name (i);
      if (ptr
	  && POINTER_TYPE_P (TREE_TYPE (ptr)))
	find_what_p_points_to (ptr);
    }

  /* Compute the call-used/clobbered sets.  */
  FOR_EACH_BB (bb)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);
	  struct pt_solution *pt;
	  if (!is_gimple_call (stmt))
	    continue;

	  pt = gimple_call_use_set (stmt);
	  if (gimple_call_flags (stmt) & ECF_CONST)
	    memset (pt, 0, sizeof (struct pt_solution));
	  else if ((vi = lookup_call_use_vi (stmt)) != NULL)
	    {
	      *pt = find_what_var_points_to (vi);
	      /* Escaped (and thus nonlocal) variables are always
	         implicitly used by calls.  */
	      /* ???  ESCAPED can be empty even though NONLOCAL
		 always escaped.  */
	      pt->nonlocal = 1;
	      pt->escaped = 1;
	    }
	  else
	    {
	      /* If there is nothing special about this call then
		 we have made everything that is used also escape.  */
	      *pt = cfun->gimple_df->escaped;
	      pt->nonlocal = 1;
	    }

	  pt = gimple_call_clobber_set (stmt);
	  if (gimple_call_flags (stmt) & (ECF_CONST|ECF_PURE|ECF_NOVOPS))
	    memset (pt, 0, sizeof (struct pt_solution));
	  else if ((vi = lookup_call_clobber_vi (stmt)) != NULL)
	    {
	      *pt = find_what_var_points_to (vi);
	      /* Escaped (and thus nonlocal) variables are always
	         implicitly clobbered by calls.  */
	      /* ???  ESCAPED can be empty even though NONLOCAL
		 always escaped.  */
	      pt->nonlocal = 1;
	      pt->escaped = 1;
	    }
	  else
	    {
	      /* If there is nothing special about this call then
		 we have made everything that is used also escape.  */
	      *pt = cfun->gimple_df->escaped;
	      pt->nonlocal = 1;
	    }
	}
    }

  timevar_pop (TV_TREE_PTA);
}


/* Delete created points-to sets.  */

static void
delete_points_to_sets (void)
{
  unsigned int i;

  shared_bitmap_table.dispose ();
  if (dump_file && (dump_flags & TDF_STATS))
    fprintf (dump_file, "Points to sets created:%d\n",
	     stats.points_to_sets_created);

  pointer_map_destroy (vi_for_tree);
  pointer_map_destroy (call_stmt_vars);
  bitmap_obstack_release (&pta_obstack);
  constraints.release ();

  for (i = 0; i < graph->size; i++)
    graph->complex[i].release ();
  free (graph->complex);

  free (graph->rep);
  free (graph->succs);
  free (graph->pe);
  free (graph->pe_rep);
  free (graph->indirect_cycles);
  free (graph);

  varmap.release ();
  free_alloc_pool (variable_info_pool);
  free_alloc_pool (constraint_pool);

  obstack_free (&fake_var_decl_obstack, NULL);

  pointer_map_destroy (final_solutions);
  obstack_free (&final_solutions_obstack, NULL);
}


/* Compute points-to information for every SSA_NAME pointer in the
   current function and compute the transitive closure of escaped
   variables to re-initialize the call-clobber states of local variables.  */

unsigned int
compute_may_aliases (void)
{
  if (cfun->gimple_df->ipa_pta)
    {
      if (dump_file)
	{
	  fprintf (dump_file, "\nNot re-computing points-to information "
		   "because IPA points-to information is available.\n\n");

	  /* But still dump what we have remaining it.  */
	  dump_alias_info (dump_file);
	}

      return 0;
    }

  /* For each pointer P_i, determine the sets of variables that P_i may
     point-to.  Compute the reachability set of escaped and call-used
     variables.  */
  compute_points_to_sets ();

  /* Debugging dumps.  */
  if (dump_file)
    dump_alias_info (dump_file);

  /* Deallocate memory used by aliasing data structures and the internal
     points-to solution.  */
  delete_points_to_sets ();

  gcc_assert (!need_ssa_update_p (cfun));

  return 0;
}

static bool
gate_tree_pta (void)
{
  return flag_tree_pta;
}

/* A dummy pass to cause points-to information to be computed via
   TODO_rebuild_alias.  */

struct gimple_opt_pass pass_build_alias =
{
 {
  GIMPLE_PASS,
  "alias",		    /* name */
  OPTGROUP_NONE,            /* optinfo_flags */
  gate_tree_pta,	    /* gate */
  NULL,                     /* execute */
  NULL,                     /* sub */
  NULL,                     /* next */
  0,                        /* static_pass_number */
  TV_NONE,                  /* tv_id */
  PROP_cfg | PROP_ssa,      /* properties_required */
  0,			    /* properties_provided */
  0,                        /* properties_destroyed */
  0,                        /* todo_flags_start */
  TODO_rebuild_alias        /* todo_flags_finish */
 }
};

/* A dummy pass to cause points-to information to be computed via
   TODO_rebuild_alias.  */

struct gimple_opt_pass pass_build_ealias =
{
 {
  GIMPLE_PASS,
  "ealias",		    /* name */
  OPTGROUP_NONE,            /* optinfo_flags */
  gate_tree_pta,	    /* gate */
  NULL,                     /* execute */
  NULL,                     /* sub */
  NULL,                     /* next */
  0,                        /* static_pass_number */
  TV_NONE,                  /* tv_id */
  PROP_cfg | PROP_ssa,      /* properties_required */
  0,			    /* properties_provided */
  0,                        /* properties_destroyed */
  0,                        /* todo_flags_start */
  TODO_rebuild_alias        /* todo_flags_finish */
 }
};


/* Return true if we should execute IPA PTA.  */
static bool
gate_ipa_pta (void)
{
  return (optimize
	  && flag_ipa_pta
	  /* Don't bother doing anything if the program has errors.  */
	  && !seen_error ());
}

/* IPA PTA solutions for ESCAPED.  */
struct pt_solution ipa_escaped_pt
  = { true, false, false, false, false, false, NULL };

/* Associate node with varinfo DATA. Worker for
   cgraph_for_node_and_aliases.  */
static bool
associate_varinfo_to_alias (struct cgraph_node *node, void *data)
{
  if ((node->symbol.alias || node->thunk.thunk_p)
      && node->symbol.analyzed)
    insert_vi_for_tree (node->symbol.decl, (varinfo_t)data);
  return false;
}

/* Execute the driver for IPA PTA.  */
static unsigned int
ipa_pta_execute (void)
{
  struct cgraph_node *node;
  struct varpool_node *var;
  int from;

  in_ipa_mode = 1;

  init_alias_vars ();

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      dump_symtab (dump_file);
      fprintf (dump_file, "\n");
    }

  /* Build the constraints.  */
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      varinfo_t vi;
      /* Nodes without a body are not interesting.  Especially do not
         visit clones at this point for now - we get duplicate decls
	 there for inline clones at least.  */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;

      gcc_assert (!node->clone_of);

      vi = create_function_info_for (node->symbol.decl,
			             alias_get_name (node->symbol.decl));
      cgraph_for_node_and_aliases (node, associate_varinfo_to_alias, vi, true);
    }

  /* Create constraints for global variables and their initializers.  */
  FOR_EACH_VARIABLE (var)
    {
      if (var->symbol.alias && var->symbol.analyzed)
	continue;

      get_vi_for_tree (var->symbol.decl);
    }

  if (dump_file)
    {
      fprintf (dump_file,
	       "Generating constraints for global initializers\n\n");
      dump_constraints (dump_file, 0);
      fprintf (dump_file, "\n");
    }
  from = constraints.length ();

  FOR_EACH_DEFINED_FUNCTION (node)
    {
      struct function *func;
      basic_block bb;

      /* Nodes without a body are not interesting.  */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;

      if (dump_file)
	{
	  fprintf (dump_file,
		   "Generating constraints for %s", cgraph_node_name (node));
	  if (DECL_ASSEMBLER_NAME_SET_P (node->symbol.decl))
	    fprintf (dump_file, " (%s)",
		     IDENTIFIER_POINTER
		       (DECL_ASSEMBLER_NAME (node->symbol.decl)));
	  fprintf (dump_file, "\n");
	}

      func = DECL_STRUCT_FUNCTION (node->symbol.decl);
      push_cfun (func);

      /* For externally visible or attribute used annotated functions use
	 local constraints for their arguments.
	 For local functions we see all callers and thus do not need initial
	 constraints for parameters.  */
      if (node->symbol.used_from_other_partition
	  || node->symbol.externally_visible
	  || node->symbol.force_output)
	{
	  intra_create_variable_infos ();

	  /* We also need to make function return values escape.  Nothing
	     escapes by returning from main though.  */
	  if (!MAIN_NAME_P (DECL_NAME (node->symbol.decl)))
	    {
	      varinfo_t fi, rvi;
	      fi = lookup_vi_for_tree (node->symbol.decl);
	      rvi = first_vi_for_offset (fi, fi_result);
	      if (rvi && rvi->offset == fi_result)
		{
		  struct constraint_expr includes;
		  struct constraint_expr var;
		  includes.var = escaped_id;
		  includes.offset = 0;
		  includes.type = SCALAR;
		  var.var = rvi->id;
		  var.offset = 0;
		  var.type = SCALAR;
		  process_constraint (new_constraint (includes, var));
		}
	    }
	}

      /* Build constriants for the function body.  */
      FOR_EACH_BB_FN (bb, func)
	{
	  gimple_stmt_iterator gsi;

	  for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi);
	       gsi_next (&gsi))
	    {
	      gimple phi = gsi_stmt (gsi);

	      if (! virtual_operand_p (gimple_phi_result (phi)))
		find_func_aliases (phi);
	    }

	  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	    {
	      gimple stmt = gsi_stmt (gsi);

	      find_func_aliases (stmt);
	      find_func_clobbers (stmt);
	    }
	}

      pop_cfun ();

      if (dump_file)
	{
	  fprintf (dump_file, "\n");
	  dump_constraints (dump_file, from);
	  fprintf (dump_file, "\n");
	}
      from = constraints.length ();
    }

  /* From the constraints compute the points-to sets.  */
  solve_constraints ();

  /* Compute the global points-to sets for ESCAPED.
     ???  Note that the computed escape set is not correct
     for the whole unit as we fail to consider graph edges to
     externally visible functions.  */
  ipa_escaped_pt = find_what_var_points_to (get_varinfo (escaped_id));

  /* Make sure the ESCAPED solution (which is used as placeholder in
     other solutions) does not reference itself.  This simplifies
     points-to solution queries.  */
  ipa_escaped_pt.ipa_escaped = 0;

  /* Assign the points-to sets to the SSA names in the unit.  */
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      tree ptr;
      struct function *fn;
      unsigned i;
      varinfo_t fi;
      basic_block bb;
      struct pt_solution uses, clobbers;
      struct cgraph_edge *e;

      /* Nodes without a body are not interesting.  */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;

      fn = DECL_STRUCT_FUNCTION (node->symbol.decl);

      /* Compute the points-to sets for pointer SSA_NAMEs.  */
      FOR_EACH_VEC_ELT (*fn->gimple_df->ssa_names, i, ptr)
	{
	  if (ptr
	      && POINTER_TYPE_P (TREE_TYPE (ptr)))
	    find_what_p_points_to (ptr);
	}

      /* Compute the call-use and call-clobber sets for all direct calls.  */
      fi = lookup_vi_for_tree (node->symbol.decl);
      gcc_assert (fi->is_fn_info);
      clobbers
	= find_what_var_points_to (first_vi_for_offset (fi, fi_clobbers));
      uses = find_what_var_points_to (first_vi_for_offset (fi, fi_uses));
      for (e = node->callers; e; e = e->next_caller)
	{
	  if (!e->call_stmt)
	    continue;

	  *gimple_call_clobber_set (e->call_stmt) = clobbers;
	  *gimple_call_use_set (e->call_stmt) = uses;
	}

      /* Compute the call-use and call-clobber sets for indirect calls
	 and calls to external functions.  */
      FOR_EACH_BB_FN (bb, fn)
	{
	  gimple_stmt_iterator gsi;

	  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	    {
	      gimple stmt = gsi_stmt (gsi);
	      struct pt_solution *pt;
	      varinfo_t vi;
	      tree decl;

	      if (!is_gimple_call (stmt))
		continue;

	      /* Handle direct calls to external functions.  */
	      decl = gimple_call_fndecl (stmt);
	      if (decl
		  && (!(fi = lookup_vi_for_tree (decl))
		      || !fi->is_fn_info))
		{
		  pt = gimple_call_use_set (stmt);
		  if (gimple_call_flags (stmt) & ECF_CONST)
		    memset (pt, 0, sizeof (struct pt_solution));
		  else if ((vi = lookup_call_use_vi (stmt)) != NULL)
		    {
		      *pt = find_what_var_points_to (vi);
		      /* Escaped (and thus nonlocal) variables are always
			 implicitly used by calls.  */
		      /* ???  ESCAPED can be empty even though NONLOCAL
			 always escaped.  */
		      pt->nonlocal = 1;
		      pt->ipa_escaped = 1;
		    }
		  else
		    {
		      /* If there is nothing special about this call then
			 we have made everything that is used also escape.  */
		      *pt = ipa_escaped_pt;
		      pt->nonlocal = 1;
		    }

		  pt = gimple_call_clobber_set (stmt);
		  if (gimple_call_flags (stmt) & (ECF_CONST|ECF_PURE|ECF_NOVOPS))
		    memset (pt, 0, sizeof (struct pt_solution));
		  else if ((vi = lookup_call_clobber_vi (stmt)) != NULL)
		    {
		      *pt = find_what_var_points_to (vi);
		      /* Escaped (and thus nonlocal) variables are always
			 implicitly clobbered by calls.  */
		      /* ???  ESCAPED can be empty even though NONLOCAL
			 always escaped.  */
		      pt->nonlocal = 1;
		      pt->ipa_escaped = 1;
		    }
		  else
		    {
		      /* If there is nothing special about this call then
			 we have made everything that is used also escape.  */
		      *pt = ipa_escaped_pt;
		      pt->nonlocal = 1;
		    }
		}

	      /* Handle indirect calls.  */
	      if (!decl
		  && (fi = get_fi_for_callee (stmt)))
		{
		  /* We need to accumulate all clobbers/uses of all possible
		     callees.  */
		  fi = get_varinfo (find (fi->id));
		  /* If we cannot constrain the set of functions we'll end up
		     calling we end up using/clobbering everything.  */
		  if (bitmap_bit_p (fi->solution, anything_id)
		      || bitmap_bit_p (fi->solution, nonlocal_id)
		      || bitmap_bit_p (fi->solution, escaped_id))
		    {
		      pt_solution_reset (gimple_call_clobber_set (stmt));
		      pt_solution_reset (gimple_call_use_set (stmt));
		    }
		  else
		    {
		      bitmap_iterator bi;
		      unsigned i;
		      struct pt_solution *uses, *clobbers;

		      uses = gimple_call_use_set (stmt);
		      clobbers = gimple_call_clobber_set (stmt);
		      memset (uses, 0, sizeof (struct pt_solution));
		      memset (clobbers, 0, sizeof (struct pt_solution));
		      EXECUTE_IF_SET_IN_BITMAP (fi->solution, 0, i, bi)
			{
			  struct pt_solution sol;

			  vi = get_varinfo (i);
			  if (!vi->is_fn_info)
			    {
			      /* ???  We could be more precise here?  */
			      uses->nonlocal = 1;
			      uses->ipa_escaped = 1;
			      clobbers->nonlocal = 1;
			      clobbers->ipa_escaped = 1;
			      continue;
			    }

			  if (!uses->anything)
			    {
			      sol = find_what_var_points_to
				      (first_vi_for_offset (vi, fi_uses));
			      pt_solution_ior_into (uses, &sol);
			    }
			  if (!clobbers->anything)
			    {
			      sol = find_what_var_points_to
				      (first_vi_for_offset (vi, fi_clobbers));
			      pt_solution_ior_into (clobbers, &sol);
			    }
			}
		    }
		}
	    }
	}

      fn->gimple_df->ipa_pta = true;
    }

  delete_points_to_sets ();

  in_ipa_mode = 0;

  return 0;
}

struct simple_ipa_opt_pass pass_ipa_pta =
{
 {
  SIMPLE_IPA_PASS,
  "pta",		                /* name */
  OPTGROUP_NONE,                        /* optinfo_flags */
  gate_ipa_pta,			/* gate */
  ipa_pta_execute,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_IPA_PTA,		        /* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_update_ssa                       /* todo_flags_finish */
 }
};
