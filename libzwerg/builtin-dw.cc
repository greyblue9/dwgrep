/*
   Copyright (C) 2018 Petr Machata
   Copyright (C) 2014 Red Hat, Inc.
   This file is part of dwgrep.

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   dwgrep is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.  */

#include <memory>
#include <sstream>

#include "atval.hh"
#include "builtin-cst.hh"
#include "builtin-dw.hh"
#include "builtin-dw-abbrev.hh"
#include "builtin.hh"
#include "dwcst.hh"
#include "dwit.hh"
#include "dwmods.hh"
#include "dwpp.hh"
#include "known-dwarf.h"
#include "known-dwarf-macro-gnu.h"
#include "op.hh"
#include "overload.hh"
#include "value-closure.hh"
#include "value-cst.hh"
#include "value-str.hh"
#include "value-dw.hh"
#include "cache.hh"

// dwopen
namespace
{
  struct op_dwopen_str
    : public op_once_overload <value_dwarf, value_str>
  {
    using op_once_overload::op_once_overload;

    value_dwarf
    operate (std::unique_ptr <value_str> a) override
    {
      return value_dwarf (a->get_string (), 0, doneness::cooked);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a string, which is interpreted as a file name, that file is
opened, and a Dwarf value representing that file is yielded::

	$ dwgrep '"dwgrep/dwgrep" dwopen unit'
	CU 0

)docstring";
    }
  };
}


// unit
namespace
{
  bool
  next_acceptable_unit (doneness d, cu_iterator &it)
  {
    if (d == doneness::raw)
      return true;

    for (; it != cu_iterator::end (); ++it)
      // In cooked mode, we reject partial units.
      // XXX Should we reject type units as well?
      if (dwarf_tag (*it) != DW_TAG_partial_unit)
	return true;

    return false;
  }

  struct dwarf_unit_producer
    : public value_producer <value_cu>
  {
    std::shared_ptr <dwfl_context> m_dwctx;
    std::vector <Dwarf *> m_dwarfs;
    std::vector <Dwarf *>::iterator m_it;
    cu_iterator m_cuit;
    size_t m_i;
    doneness m_doneness;

    dwarf_unit_producer (std::shared_ptr <dwfl_context> dwctx, doneness d)
      : m_dwctx {dwctx}
      , m_dwarfs {all_dwarfs (*dwctx)}
      , m_it {m_dwarfs.begin ()}
      , m_cuit {cu_iterator::end ()}
      , m_i {0}
      , m_doneness {d}
    {}

    std::unique_ptr <value_cu>
    next () override
    {
      do
	if (! maybe_next_dwarf (m_cuit, m_it, m_dwarfs.end ()))
	  return nullptr;
      while (! next_acceptable_unit (m_doneness, m_cuit));

      Dwarf_CU &cu = *(*m_cuit)->cu;
      Dwarf_Off off = m_cuit.offset ();
      m_cuit++;

      return std::make_unique <value_cu> (m_dwctx, cu, off, m_i++, m_doneness);
    }
  };

  struct op_unit_dwarf
    : public op_yielding_overload <value_cu, value_dwarf>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_cu>>
    operate (std::unique_ptr <value_dwarf> a) override
    {
      return std::make_unique <dwarf_unit_producer> (a->get_dwctx (),
						     a->get_doneness ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Take a Dwarf on TOS and yield units defined therein.  In raw mode,
yields all units without exception, in cooked mode it only yields
actual compile units (i.e. those units whose CU DIE's tag is
``DW_TAG_compile_unit``.

For example::

	$ dwgrep ./tests/dwz-partial -e 'raw unit root "%s"'
	[b] partial_unit
	[34] compile_unit
	[a4] compile_unit
	[e1] compile_unit
	[11e] compile_unit

	$ dwgrep ./tests/dwz-partial -e 'cooked unit root "%s"'
	[34] compile_unit
	[a4] compile_unit
	[e1] compile_unit
	[11e] compile_unit

This operator is identical in operation to the following expression::

	entry ?root unit

)docstring";
    }
  };

  value_cu
  cu_for_die (std::shared_ptr <dwfl_context> dwctx, Dwarf_Die die, doneness d)
  {
    Dwarf_Die cudie;
    if (dwarf_diecu (&die, &cudie, nullptr, nullptr) == nullptr)
      throw_libdw ();

    Dwarf *dw = dwarf_cu_getdwarf (cudie.cu);
    cu_iterator cuit {dw, cudie};

    return value_cu (dwctx, *(*cuit)->cu, cuit.offset (), 0, d);
  }

  struct op_unit_die
    : public op_once_overload <value_cu, value_die>
  {
    using op_once_overload::op_once_overload;

    value_cu
    operate (std::unique_ptr <value_die> a) override
    {
      return cu_for_die (a->get_dwctx (), a->get_die (), a->get_doneness ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Take a DIE on TOS and yield a unit that this DIE belongs to::

	$ dwgrep ./tests/twocus -e 'unit'
	CU 0
	CU 0x53

	$ dwgrep ./tests/twocus -e 'entry (offset == 0x4b) unit'
	CU 0

)docstring";
    }
  };

  struct op_unit_attr
    : public op_once_overload <value_cu, value_attr>
  {
    using op_once_overload::op_once_overload;

    value_cu
    operate (std::unique_ptr <value_attr> a) override
    {
      return cu_for_die (a->get_dwctx (), a->get_die (), doneness::cooked);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Take an attribute on TOS and yield a unit where the attribute
originated::

	$ dwgrep ./tests/twocus -e 'unit'
	CU 0
	CU 0x53

	$ dwgrep ./tests/twocus -e 'entry (offset == 0xb3) attribute unit'
	CU 0x53
	CU 0x53
	CU 0x53

)docstring";
    }
  };
}

// entry
namespace
{
  template <class It>
  std::pair <It, It> get_it_range (Dwarf_Die cudie, bool skip);

  template <>
  std::pair <all_dies_iterator, all_dies_iterator>
  get_it_range (Dwarf_Die cudie, bool skip)
  {
    Dwarf *dw = dwarf_cu_getdwarf (cudie.cu);
    cu_iterator cuit {dw, cudie};
    all_dies_iterator a (cuit);
    all_dies_iterator e (++cuit);
    if (skip)
      ++a;
    return std::make_pair (a, e);
  }

  template <>
  std::pair <child_iterator, child_iterator>
  get_it_range (Dwarf_Die cudie, bool skip)
  {
    // N.B. this always skips the passed-in DIE.
    child_iterator a {cudie};
    return std::make_pair (a, child_iterator::end ());
  }

  template <class It>
  bool
  import_partial_units (std::vector <std::pair <It, It>> &stack,
			std::shared_ptr <dwfl_context> dwctx,
			std::shared_ptr <value_die> &import)
  {
    Dwarf_Die *die = *stack.back ().first;
    Dwarf_Attribute at_import;
    Dwarf_Die cudie;
    if (dwarf_tag (die) == DW_TAG_imported_unit
	&& dwarf_hasattr (die, DW_AT_import)
	&& dwarf_attr (die, DW_AT_import, &at_import) != nullptr
	&& dwarf_formref_die (&at_import, &cudie) != nullptr)
      {
	// Do this first, before we bump the iterator and DIE gets
	// invalidated.
	import = std::make_shared <value_die> (dwctx, import, *die, 0,
					       doneness::cooked);

	// Skip DW_TAG_imported_unit.
	stack.back ().first++;

	// `true` to skip root DIE of DW_TAG_partial_unit.
	stack.push_back (get_it_range <It> (cudie, true));
	return true;
      }

    return false;
  }

  template <class It>
  bool
  drop_finished_imports (std::vector <std::pair <It, It>> &stack,
			 std::shared_ptr <value_die> &import)
  {
    assert (! stack.empty ());
    if (stack.back ().first != stack.back ().second)
      return false;

    stack.pop_back ();

    // We have one more item in STACK than values in IMPORT chain, so
    // this can actually be empty at this point.
    if (import != nullptr)
      import = import->get_import ();

    return true;
  }

  // This producer encapsulates the logic for iteration through a
  // range of DIE's, with optional inlining of partial units along the
  // way.  Cooked producers do inline, raw ones don't.
  template <class It>
  struct die_it_producer
    : public value_producer <value_die>
  {
    std::shared_ptr <dwfl_context> m_dwctx;

    // Stack of iterator ranges.
    std::vector <std::pair <It, It>> m_stack;

    // Chain of DIE's where partial units were imported.
    std::shared_ptr <value_die> m_import;

    size_t m_i;
    doneness m_doneness;

    die_it_producer (std::shared_ptr <dwfl_context> dwctx, Dwarf_Die die,
		     doneness d)
      : m_dwctx {dwctx}
      , m_i {0}
      , m_doneness {d}
    {
      m_stack.push_back (get_it_range <It> (die, false));
    }

    std::unique_ptr <value_die>
    next () override
    {
      do
	if (m_stack.empty ())
	  return nullptr;
      while (drop_finished_imports (m_stack, m_import)
	     || (m_doneness == doneness::cooked
		 && import_partial_units (m_stack, m_dwctx, m_import)));

      return std::make_unique <value_die>
	(m_dwctx, m_import, **m_stack.back ().first++, m_i++, m_doneness);
    }
  };

  std::unique_ptr <value_producer <value_die>>
  make_cu_entry_producer (std::shared_ptr <dwfl_context> dwctx, Dwarf_CU &cu,
			  doneness d)
  {
    return std::make_unique <die_it_producer <all_dies_iterator>>
      (dwctx, dwpp_cudie (cu), d);
  }

  struct op_entry_cu
    : public op_yielding_overload <value_die, value_cu>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_die>>
    operate (std::unique_ptr <value_cu> a) override
    {
      return make_cu_entry_producer (a->get_dwctx (), a->get_cu (),
				     a->get_doneness ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yield DIE's that it defines::

	$ dwgrep ./tests/twocus -e 'unit (offset == 0x53) entry "%s"'
	[5e] compile_unit
	[80] subprogram
	[a3] subprogram
	[b0] unspecified_parameters
	[b3] base_type

This operator is similar in operation to the following::

	root child*

The difference may lie (and as of this writing lies) in the order in
which the entries are yielded.

In cooked mode, it resolves any ``DW_TAG_imported_unit``'s that it
finds during exploring the DIE tree.  One entry can thus be explored
several times, each time in a different context::

	$ dwgrep ./tests/dwz-partial -e 'entry (offset == 0x14) (|A| A A parent* ?root) "%s inside %s"'
	[14] pointer_type inside [34] compile_unit
	[14] pointer_type inside [a4] compile_unit
	[14] pointer_type inside [e1] compile_unit
	[14] pointer_type inside [11e] compile_unit

	$ dwgrep ./tests/dwz-partial -e 'raw entry (offset == 0x14) (|A| A A parent* ?root) "%s inside %s"'
	[14] pointer_type inside [b] partial_unit

)docstring";
    }
  };

  template <class A, class B>
  struct op_entry_dwarf_base
    : public stub_op
  {
    op_entry_dwarf_base (std::shared_ptr <op> upstream)
      : stub_op {std::make_shared <B> (std::make_shared <A> (upstream))}
    {}

    stack::uptr
    next () override
    { return m_upstream->next (); }
  };

  struct op_entry_dwarf
    : public op_entry_dwarf_base <op_unit_dwarf, op_entry_cu>
  {
    using op_entry_dwarf_base::op_entry_dwarf_base;

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a Dwarf on TOS and yields DIE's that it contains.  This operator
behaves equivalently to tho following::

	unit entry

)docstring";
    }

    static builtin_protomap
    protomap ()
    {
      return { builtin_prototype ({value_dwarf::vtype}, yield::many,
				  {value_die::vtype}) };
    }

    static selector get_selector ()
    { return {value_dwarf::vtype}; }
  };
}

// child
namespace
{
  std::unique_ptr <value_producer <value_die>>
  make_die_child_producer (std::shared_ptr <dwfl_context> dwctx,
			   Dwarf_Die parent, doneness d)
  {
    return std::make_unique <die_it_producer <child_iterator>>
      (dwctx, parent, d);
  }

  struct op_child_die
    : public op_yielding_overload <value_die, value_die>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_die>>
    operate (std::unique_ptr <value_die> a) override
    {
      return make_die_child_producer (a->get_dwctx (), a->get_die (),
				      a->get_doneness ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields all its children.  If the taken DIE is
cooked, and one of the children is a ``DW_TAG_imported_unit``, the
operator resolves that reference and recursively yields also all its
children::

	$ dwgrep ./tests/dwz-partial2-1 -e 'unit root (offset == 0xe7) raw child "%s"'
	[f9] imported_unit
	[fe] imported_unit
	[103] variable

	$ dwgrep ./tests/dwz-partial2-1 -e 'unit root (offset == 0xe7) child "%s"'
	[76] base_type
	[7d] base_type
	[4b] typedef
	[51] pointer_type
	[54] pointer_type
	[57] pointer_type
	[5a] const_type
	[5c] volatile_type
	[103] variable

)docstring";
    }
  };
}

// elem, relem
namespace
{
  struct elem_loclist_producer
    : public value_producer <value_loclist_op>
  {
    std::unique_ptr <value_loclist_elem> m_value;
    size_t m_i;
    size_t m_n;
    bool m_forward;

    elem_loclist_producer (std::unique_ptr <value_loclist_elem> value,
			   bool forward)
      : m_value {std::move (value)}
      , m_i {0}
      , m_n {m_value->get_exprlen ()}
      , m_forward {forward}
    {}

    std::unique_ptr <value_loclist_op>
    next () override
    {
      size_t idx = m_i++;
      if (idx < m_n)
	{
	  if (! m_forward)
	    idx = m_n - 1 - idx;
	  return std::make_unique <value_loclist_op>
	    (m_value->get_dwctx (), m_value->get_attr (),
	     m_value->get_expr () + idx, idx);
	}
      else
	return nullptr;
    }
  };

  char const elem_loclist_docstring[] =
R"docstring(

Takes a location expression on TOS and yields individual operators::

	$ dwgrep ./tests/testfile_const_type -e 'entry (offset == 0x57)'
	[57]	variable
		name (string)	w;
		decl_file (data1)	/home/mark/src/elfutils/tests/const_type.c;
		decl_line (data1)	6;
		type (ref4)	[25];
		location (exprloc)	0..0xffffffffffffffff:[0:fbreg<0>, 2:GNU_deref_type<8>/<37>, [... snip ...]];

	$ dwgrep ./tests/testfile_const_type -e 'entry (offset == 0x57) @AT_location elem'
	0:fbreg<0>
	2:GNU_deref_type<8>/<37>
	5:GNU_const_type<[25] base_type>/<[0, 0, 0x80, 0x67, 0x45, 0x23, 0x1, 0]>
	16:div
	17:GNU_convert<44>
	19:stack_value

``relem`` yields elements backwards.

)docstring";

  struct op_elem_loclist_elem
    : public op_yielding_overload <value_loclist_op, value_loclist_elem>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_loclist_op>>
    operate (std::unique_ptr <value_loclist_elem> a) override
    {
      return std::make_unique <elem_loclist_producer> (std::move (a), true);
    }

    static std::string
    docstring ()
    {
      return elem_loclist_docstring;
    }
  };

  struct op_relem_loclist_elem
    : public op_yielding_overload <value_loclist_op, value_loclist_elem>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_loclist_op>>
    operate (std::unique_ptr <value_loclist_elem> a) override
    {
      return std::make_unique <elem_loclist_producer> (std::move (a), false);
    }

    static std::string
    docstring ()
    {
      return elem_loclist_docstring;
    }
  };

  struct elem_aset_producer
    : public value_producer <value_cst>
  {
    coverage cov;
    size_t m_idx;	// position among ranges
    uint64_t m_ai;	// iteration through a range
    size_t m_i;		// produced value counter
    bool m_forward;

    elem_aset_producer (coverage a_cov, bool forward)
      : cov {a_cov}
      , m_idx {0}
      , m_ai {0}
      , m_i {0}
      , m_forward {forward}
    {}

    std::unique_ptr <value_cst>
    next () override
    {
      if (m_idx >= cov.size ())
	return nullptr;

      auto idx = [&] () {
	return m_forward ? m_idx : cov.size () - 1 - m_idx;
      };

      if (m_ai >= cov.at (idx ()).length)
	{
	  m_idx++;
	  if (m_idx >= cov.size ())
	    return nullptr;
	  assert (cov.at (idx ()).length > 0);
	  m_ai = 0;
	}

      uint64_t ai = m_forward ? m_ai : cov.at (idx ()).length - 1 - m_ai;
      uint64_t addr = cov.at (idx ()).start + ai;
      m_ai++;

      return std::make_unique <value_cst>
	(constant {addr, &dw_address_dom ()}, m_i++);
    }
  };

  const char elem_aset_docstring[] =
R"docstring(

Take an address set on TOS and yield all its addresses.  Be warned
that this may be a very expensive operation.  It is common that
address sets cover the whole address range, which for a 64-bit
architecture is a whole lot of addresses.

Example::

	$ dwgrep ./tests/testfile_const_type -e 'entry (name == "main") address'
	[0x80482f0, 0x80482f3)

	$ dwgrep ./tests/testfile_const_type -e 'entry (name == "main") address elem'
	0x80482f0
	0x80482f1
	0x80482f2

``relem`` behaves similarly, but yields addresses in reverse order.

)docstring";

  struct op_elem_aset
    : public op_yielding_overload <value_cst, value_aset>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_cst>>
    operate (std::unique_ptr <value_aset> val) override
    {
      return std::make_unique <elem_aset_producer>
	(val->get_coverage (), true);
    }

    static std::string
    docstring ()
    {
      return elem_aset_docstring;
    }
  };

  struct op_relem_aset
    : public op_yielding_overload <value_cst, value_aset>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_cst>>
    operate (std::unique_ptr <value_aset> val) override
    {
      return std::make_unique <elem_aset_producer>
	(val->get_coverage (), false);
    }

    static std::string
    docstring ()
    {
      return elem_aset_docstring;
    }
  };
}

// attribute
namespace
{
  bool
  attr_should_be_integrated (int code)
  {
    // Some attributes only make sense at the non-defining DIE and
    // shouldn't be brought down through DW_AT_specification or
    // DW_AT_abstract_origin.

    // Note: DW_AT_decl_* suite should normally be integrated.  GCC
    // will only emit the unique attributes at concrete instance,
    // leading to DIE's that e.g. only have DW_AT_decl_line, because
    // DW_AT_decl_file is inherited.

    switch (code)
      {
      case DW_AT_sibling:
      case DW_AT_declaration:
	return false;

      default:
	return true;
      }
  }

  struct attribute_producer
    : public value_producer <value_attr>
  {
    std::shared_ptr <dwfl_context> m_dwctx;
    Dwarf_Die m_die;
    attr_iterator m_it;
    size_t m_i;
    doneness m_doneness;
    bool m_secondary;

    // Already seen attributes.
    std::vector <int> m_seen;

    // We store full DIE's to allow DW_AT_specification's in a
    // separate debug info files.
    std::vector <Dwarf_Die> m_next;

    void
    schedule (Dwarf_Attribute &at)
    {
      assert (at.code == DW_AT_abstract_origin
	      || at.code == DW_AT_specification);

      Dwarf_Die die_mem;
      if (dwarf_formref_die (&at, &die_mem) == nullptr)
	throw_libdw ();

      m_next.push_back (die_mem);
    }

    bool
    next_die ()
    {
      if (m_next.empty ())
	return false;

      m_die = m_next.back ();
      m_it = attr_iterator {&m_die};
      m_next.pop_back ();
      return true;
    }

    bool
    seen (int atname) const
    {
      return std::find (std::begin (m_seen), std::end (m_seen),
			atname) != std::end (m_seen);
    }

    attribute_producer (std::unique_ptr <value_die> value)
      : m_dwctx {value->get_dwctx ()}
      , m_it {attr_iterator::end ()}
      , m_i {0}
      , m_doneness {value->get_doneness ()}
      , m_secondary {false}
    {
      m_next.push_back (value->get_die ());
      next_die ();
    }

    std::unique_ptr <value_attr>
    next () override
    {
      Dwarf_Attribute at;
      bool const integrate = m_doneness == doneness::cooked;
      do
	{
	again:
	  while (m_it == attr_iterator::end ())
	    if (! integrate || ! next_die ())
	      return nullptr;
	    else
	      m_secondary = true;

	  at = **m_it++;
	  if (integrate
	      && (at.code == DW_AT_specification
		  || at.code == DW_AT_abstract_origin))
	    {
	      // Schedule this for future traversal, but still show
	      // the attribute in the output (i.e. skip the seen-check
	      // to possibly also present this several times if we
	      // went through several rounds of integration).  There's
	      // no gain in hiding this from the user.
	      schedule (at);
	      break;
	    }

	  if (m_secondary && ! attr_should_be_integrated (at.code))
	    goto again;
	}
      while (integrate && seen (at.code));

      m_seen.push_back (at.code);
      return std::make_unique <value_attr>
		(m_dwctx, at, m_die, m_i++, m_doneness);
    }
  };

  struct op_attribute_die
    : public op_yielding_overload <value_attr, value_die>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value_attr>>
    operate (std::unique_ptr <value_die> a) override
    {
      return std::make_unique <attribute_producer> (std::move (a));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields its individual attributes.  If it is a
raw DIE, it yields only those attributes that the DIE contains.  For
cooked DIE's it also integrates attributes at DIE's referenced by
``DW_AT_abstract_origin`` and ``DW_AT_specification``, except those
that are already present at the original DIE, or those that it makes
no sense to import (as of this writing, ``DW_AT_sibling``,
``DW_AT_declaration``).

Example::

	$ dwgrep ./tests/nullptr.o -e 'raw entry (offset == 0x6e) attribute'
	specification (ref4)	[3f];
	inline (data1)	DW_INL_declared_inlined;
	object_pointer (ref4)	[7c];
	sibling (ref4)	[8b];

	$ dwgrep ./tests/nullptr.o -e 'raw entry (offset == 0x3f)'
	[3f]	subprogram
		external (flag_present)	true;			# Will be integrate.
		name (string)	foo;				# Likewise.
		decl_file (data1)	tests/nullptr.cc;	# Likewise.
		decl_line (data1)	3;			# Likewise.
		declaration (flag_present)	true;		# Won't--meaningless.
		object_pointer (ref4)	[4a];			# Won't--duplicate.

	$ dwgrep ./tests/nullptr.o -e 'entry (offset == 0x6e) attribute'
	specification (ref4)	[3f];
	inline (data1)	DW_INL_declared_inlined;
	object_pointer (ref4)	[7c];
	sibling (ref4)	[8b];
	external (flag_present)	true;
	name (string)	foo;
	decl_file (data1)	tests/nullptr.cc;
	decl_line (data1)	3;

)docstring";
    }
  };
}

// offset
namespace
{
  struct op_offset_cu
    : public op_once_overload <value_cst, value_cu>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_cu> a) override
    {
      return value_cst {constant {a->get_offset (), &dw_offset_dom ()}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yields its offset inside the Dwarf section that
it comes from::

	$ dwgrep tests/twocus -e 'unit offset'
	0
	0x53

Note that CU offset is different from the offset of its root element::

	$ dwgrep tests/twocus -e 'unit root offset'
	0xb
	0x5e

|OffsetsNotUnique|

.. |OffsetsNotUnique| replace::
    Note that offset does not uniquely identify a Dwarf object, as
    offsets of objects from .gnu_debugaltlink files are again numbered
    from 0, and may conflict with those from the main file.  Direct
    comparison of individual objects will not be fooled though.

Example::

	$ dwgrep tests/a1.out -e '
		let A := raw unit (pos == 0);
		let B := raw unit (pos == 1);
		A B if (A == B) then "eq" else "ne"'
	---
	ne
	CU 0
	CU 0
	<Dwarf "tests/a1.out">

)docstring";
    }
  };

  struct op_offset_die
    : public op_once_overload <value_cst, value_die>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_die> val) override
    {
      constant c {dwarf_dieoffset (&val->get_die ()), &dw_offset_dom ()};
      return value_cst {c, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields its offset inside the Dwarf section that
it comes from::

	$ dwgrep tests/twocus -e 'unit root offset'
	0xb
	0x5e

|OffsetsNotUnique|

Also, DIE's brought in through imports will report the same offset
even though they came in through different paths.  E.g. if a single
DIE is selected in the same context in two different expressions, the
two DIE's compare equal::

	$ dwgrep tests/dwz-partial2-1 -e '
		let A := [entry (offset == 0x14)] elem (pos == 0);
		let B := [entry (offset == 0x14)] elem (pos == 0);
		A B if ?eq then "==" else "!=" swap "%s: %s %s %s"'
	<Dwarf "tests/dwz-partial2-1">: [14] typedef == [14] typedef

However if the two DIE's are taken from different contexts, they will
compare unequal despite them being physically the same DIE::

	$ dwgrep tests/dwz-partial2-1 -e '
		let A := [entry (offset == 0x14)] elem (pos == 0);
		let B := [entry (offset == 0x14)] elem (pos == 1);
		A B if ?eq then "==" else "!=" swap "%s: %s %s %s"'
	<Dwarf "tests/dwz-partial2-1">: [14] typedef != [14] typedef

)docstring";
    }
  };

  struct op_offset_loclist_op
    : public op_once_overload <value_cst, value_loclist_op>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_loclist_op> val) override
    {
      Dwarf_Op const *dwop = val->get_dwop ();
      return value_cst {constant {dwop->offset, &dw_offset_dom ()}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a location list operation on TOS and yields its offset within
the containing location expression::

	$ dwgrep ./tests/testfile_const_type -e 'entry (offset == 0x57)'
	[57]	variable
		name (string)	w;
		decl_file (data1)	/home/mark/src/elfutils/tests/const_type.c;
		decl_line (data1)	6;
		type (ref4)	[25];
		location (exprloc)	0..0xffffffffffffffff:[0:fbreg<0>, 2:GNU_deref_type<8>/<37>, [... snip ...]];

	$ dwgrep ./tests/testfile_const_type -e 'entry (offset == 0x57) @AT_location elem offset'
	0
	0x2
	0x5
	0x10
	0x11
	0x13

)docstring";
    }
  };
}

// address
namespace
{
  struct op_address_die
    : public op_once_overload <value_aset, value_die>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_die> a) override
    {
      return die_ranges (a->get_die ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields an address set describing address ranges
covered by this DIE.  (This calls dwarf_ranges under the covers.)::

	$ dwgrep ./tests/testfile_const_type -e 'entry (name == "main") address'
	[0x80482f0, 0x80482f3)

)docstring";
    }
  };

  std::unique_ptr <value_cst>
  get_die_addr (Dwarf_Die &die, int (cb) (Dwarf_Die *, Dwarf_Addr *))
  {
    Dwarf_Addr addr;
    if (cb (&die, &addr) < 0)
      throw_libdw ();
    return std::make_unique <value_cst>
      (constant {addr, &dw_address_dom ()}, 0);
  }

  std::unique_ptr <value_cst>
  maybe_get_die_addr (Dwarf_Die &die, int atname,
		      int (cb) (Dwarf_Die *, Dwarf_Addr *))
  {
    // Attributes that hold addresses shouldn't generally be found
    // in abstract origins and declarations, so we don't need to
    // concern ourselves with integrating here.
    if (! dwarf_hasattr (&die, atname))
      return nullptr;
    return get_die_addr (die, cb);
  }

  struct op_address_attr
    : public op_overload <value_cst, value_attr>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_cst>
    operate (std::unique_ptr <value_attr> a) override
    {
      if (dwarf_whatattr (&a->get_attr ()) == DW_AT_high_pc)
	return get_die_addr (a->get_die (), &dwarf_highpc);

      if (dwarf_whatattr (&a->get_attr ()) == DW_AT_entry_pc)
	return get_die_addr (a->get_die (), &dwarf_entrypc);

      if (dwarf_whatform (&a->get_attr ()) == DW_FORM_addr)
	{
	  Dwarf_Addr addr;
	  if (dwarf_formaddr (&a->get_attr (), &addr) < 0)
	    throw_libdw ();
	  return std::make_unique <value_cst>
	    (constant {addr, &dw_address_dom ()}, 0);
	}

      std::cerr << "`address' applied to non-address attribute:\n    ";
      a->show (std::cerr, brevity::brief);
      std::cerr << std::endl;

      return nullptr;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE attribute on TOS and yields an address that it references.
For attributes with an address form, this simply yields the value of
the attribute.  For ``DW_AT_high_pc`` and ``DW_AT_entry_pc``
attributes with a constant form, this converts the relative constant
value to absolute address::

	$ dwgrep ./tests/bitcount.o -e 'unit root'
	[b]	compile_unit
		[... snip ...]
		low_pc (addr)	0x10000;
		high_pc (data8)	32;
		stmt_list (sec_offset)	0;

	$ dwgrep ./tests/bitcount.o -e 'unit root attribute ?AT_high_pc address'
	0x10020

)docstring";
    }
  };

  struct op_address_loclist_elem
    : public op_once_overload <value_aset, value_loclist_elem>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_loclist_elem> val) override
    {
      uint64_t low = val->get_low ();
      uint64_t len = val->get_high () - low;

      coverage cov;
      cov.add (low, len);
      return value_aset {cov, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a location list element on TOS and yields an address set
describing where it is valid::

	$ dwgrep ./tests/bitcount.o -e 'entry (offset == 0x91)'
	[91]	formal_parameter
		name (string)	u;
		decl_file (data1)	tests/bitcount.c;
		decl_line (data1)	3;
		type (ref4)	[5e];
		location (sec_offset)	0x10000..0x10017:[0:reg5];0x10017..0x1001a:[0:breg5<0>, 2:breg1<0>, 4:and, 5:stack_value];0x1001a..0x10020:[0:reg5];

	$ dwgrep ./tests/bitcount.o -e 'entry (offset == 0x91) @AT_location address'
	[0x10000, 0x10017)
	[0x10017, 0x1001a)
	[0x1001a, 0x10020)

)docstring";
    }
  };
}

// label
namespace
{
  struct op_label_die
    : public op_once_overload <value_cst, value_die>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_die> val) override
    {
      int tag = dwarf_tag (&val->get_die ());
      return value_cst {constant {tag, &dw_tag_dom ()}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields its tag::

	$ dwgrep ./tests/a1.out -e 'raw unit root label'
	DW_TAG_compile_unit
	DW_TAG_partial_unit

)docstring";
    }
  };

  struct op_label_attr
    : public op_once_overload <value_cst, value_attr>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_attr> val) override
    {
      constant cst {dwarf_whatattr (&val->get_attr ()), &dw_attr_dom ()};
      return value_cst {cst, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an attribute on TOS and yields its name::

	$ dwgrep ./tests/a1.out -e 'unit root attribute label'
	DW_AT_producer
	DW_AT_language
	DW_AT_name
	DW_AT_comp_dir
	DW_AT_low_pc
	DW_AT_high_pc
	DW_AT_stmt_list

)docstring";
    }
  };

  struct op_label_loclist_op
    : public op_once_overload <value_cst, value_loclist_op>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_loclist_op> val) override
    {
      constant cst {val->get_dwop ()->atom, &dw_locexpr_opcode_dom (),
		    brevity::brief};
      return value_cst {cst, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a location expression instruction on TOS and yields the
operator::

	$ dwgrep ./tests/bitcount.o -e 'entry (offset == 0x91) @AT_location elem label'
	reg5
	breg5
	breg1
	and
	stack_value
	reg5

)docstring";
    }
  };
}

// form
namespace
{
  struct op_form_attr
    : public op_once_overload <value_cst, value_attr>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_attr> val) override
    {
      constant cst {dwarf_whatform (&val->get_attr ()), &dw_form_dom ()};
      return value_cst {cst, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an attribute on TOS and yields its form.

)docstring";
    }
  };
}

// parent
namespace
{
  template <class T>
  bool
  get_parent (T &value, Dwarf_Die &ret)
  {
    Dwarf_Off par_off = value.get_dwctx ()->find_parent (value.get_die ());
    if (par_off == parent_cache::no_off)
      return false;

    if (dwarf_offdie (dwarf_cu_getdwarf (value.get_die ().cu),
		      par_off, &ret) == nullptr)
      throw_libdw ();

    return true;
  }

  struct op_parent_die
    : public op_overload <value_die, value_die>
  {
    using op_overload::op_overload;

    bool
    pop_import (std::shared_ptr <value_die> &a)
    {
      if (auto b = a->get_import ())
	{
	  a = b;
	  return true;
	}
      return false;
    }

    std::unique_ptr <value_die>
    do_operate (std::shared_ptr <value_die> a)
    {
      doneness d = a->get_doneness ();

      // Both cooked and raw DIE's have parents (unless they don't, in
      // which case we are already at root).  But for cooked DIE's,
      // when the parent is partial unit root, we need to traverse
      // further along the import chain.
      Dwarf_Die par_die;
      do
	if (! get_parent (*a, par_die))
	  return nullptr;
      while (d == doneness::cooked
	     && dwarf_tag (&par_die) == DW_TAG_partial_unit
	     && pop_import (a));

      return std::make_unique <value_die> (a->get_dwctx (), par_die, 0, d);
    }

    std::unique_ptr <value_die>
    operate (std::unique_ptr <value_die> a) override
    {
      return do_operate (std::move (a));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields its parental DIE.  For cooked DIE's, in
traverses parents across import points back to from which context the
particular DIE was explored::

	$ dwgrep ./tests/a1.out -e 'unit raw entry "%s"'
	[b] compile_unit
	[2d] imported_unit
	[32] subprogram
	[51] variable

	$ dwgrep ./tests/a1.out -e 'entry (offset == 0x14) "%s"'
	[14] typedef

	$ dwgrep ./tests/a1.out -e 'entry (offset == 0x14) parent "%s"'
	[b] compile_unit

)docstring";
    }
  };
}

// ?root
namespace
{
  struct pred_rootp_die
    : public pred_overload <value_die>
  {
    using pred_overload <value_die>::pred_overload;

    pred_result
    result (value_die &a) override
    {
      // N.B. the following works the same for raw as well as cooked
      // DIE's.  The difference in behavior is in 'parent', which for
      // cooked DIE's should never get to DW_TAG_partial_unit DIE
      // unless we've actually started traversal in that partial unit.
      // In that case it's fully legitimate that ?root holds on such
      // node.
      return pred_result (a.get_dwctx ()->is_root (a.get_die ()));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Holds for root DIE's, i.e. DIE's that don't have a parental DIE.

)docstring";
    }
  };
}

// root
namespace
{
  struct op_root_cu
    : public op_once_overload <value_die, value_cu>
  {
    using op_once_overload::op_once_overload;

    value_die
    operate (std::unique_ptr <value_cu> a) override
    {
      Dwarf_CU &cu = a->get_cu ();
      Dwarf_Die cudie;
      if (dwarf_cu_die (&cu, &cudie, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr) == nullptr)
	throw_libdw ();

      return value_die {a->get_dwctx (), cudie, 0,
			a->get_doneness ()};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yields its root DIE.

)docstring";
    }
  };

  struct op_root_die
    : public op_once_overload <value_die, value_die>
  {
    using op_once_overload::op_once_overload;

    static value_die
    do_operate (std::shared_ptr <value_die> a)
    {
      auto d = a->get_doneness ();
      if (d == doneness::cooked)
	while (a->get_import () != nullptr)
	  a = a->get_import ();

      return value_die {a->get_dwctx (),
			dwpp_cudie (a->get_die ()), 0, d};
    }

    value_die
    operate (std::unique_ptr <value_die> a) override
    {
      return do_operate (std::move (a));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields CU DIE of the unit that this DIE comes
from.  For cooked DIE's follows back through import points and yields
the CU DIE of this DIE's context.

)docstring";
    }
  };
}

// value
namespace
{
  struct op_value_attr
    : public op_yielding_overload <value, value_attr>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value>>
    operate (std::unique_ptr <value_attr> a) override
    {
      return at_value (a->get_dwctx (), a->get_die (), a->get_attr ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an attribute on TOS and yields its value.  The value can be of
any Zwerg type.  There can even be more than one value.  In particular
location expression attributes yield all the constituent location
expressions::

	$ dwgrep ./tests/aranges.o -e 'entry attribute ?AT_location value'
	0x10000..0x10010:[0:reg5]
	0x10010..0x1001a:[0:fbreg<-24>]

)docstring";
    }
  };

  struct op_value_loclist_op
    : public op_yielding_overload <value, value_loclist_op>
  {
    using op_yielding_overload::op_yielding_overload;

    std::unique_ptr <value_producer <value>>
    operate (std::unique_ptr <value_loclist_op> a) override
    {
      return std::make_unique <value_producer_cat <value>>
	(dwop_number (a->get_dwctx (), a->get_attr (), a->get_dwop ()),
	 dwop_number2 (a->get_dwctx (), a->get_attr (), a->get_dwop ()));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a location expression instruction on TOS and yields its
operands::

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location elem'
	0:reg5
	0:fbreg<-24>

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location elem value'
	-24

Operands could be of any Zwerg type, some will be e.g. DIE's.

)docstring";
    }
  };
}

// low
namespace
{
  struct op_low_die
    : public op_overload <value_cst, value_die>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_cst>
    operate (std::unique_ptr <value_die> a) override
    {
      return maybe_get_die_addr (a->get_die (), DW_AT_low_pc, &dwarf_lowpc);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Equivalent to ``@AT_low_pc``.

)docstring";
    }
  };

  struct op_low_aset
    : public op_overload <value_cst, value_aset>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_cst>
    operate (std::unique_ptr <value_aset> a) override
    {
      auto &cov = a->get_coverage ();
      if (cov.empty ())
	return nullptr;

      return std::make_unique <value_cst>
	(constant {cov.at (0).start, &dw_address_dom ()}, 0);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set on TOS and yields lowest address set in this set.
Doesn't yield at all if the set is empty::

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location'
	0x10000..0x10010:[0:reg5]
	0x10010..0x1001a:[0:fbreg<-24>]

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location address low'
	0x10000
	0x10010

)docstring";
    }
  };
}

// high
namespace
{
  struct op_high_die
    : public op_overload <value_cst, value_die>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_cst>
    operate (std::unique_ptr <value_die> a) override
    {
      return maybe_get_die_addr (a->get_die (), DW_AT_high_pc, &dwarf_highpc);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Equivalent to ``@AT_high_pc``.

)docstring";
    }
  };

  struct op_high_aset
    : public op_overload <value_cst, value_aset>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_cst>
    operate (std::unique_ptr <value_aset> a) override
    {
      auto &cov = a->get_coverage ();
      if (cov.empty ())
	return nullptr;

      auto range = cov.at (cov.size () - 1);

      return std::make_unique <value_cst>
	(constant {range.start + range.length, &dw_address_dom ()}, 0);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set on TOS and yields highest address of this set.
Note that that address doesn't actually belong to the set.  Doesn't
yield at all if the set is empty.

)docstring";
    }
  };
}

// aset
namespace
{
  mpz_class
  addressify (constant c)
  {
    if (! c.dom ()->safe_arith ())
      std::cerr << "Warning: the constant " << c
		<< " doesn't seem to be suitable for use in address sets.\n";

    auto v = c.value ();

    if (v < 0)
      {
	std::cerr
	  << "Warning: Negative values are not allowed in address sets.\n";
	v = 0;
      }

    return v;
  }

  struct op_aset_cst_cst
    : public op_once_overload <value_aset, value_cst, value_cst>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_cst> a,
	     std::unique_ptr <value_cst> b) override
    {
      auto av = addressify (a->get_constant ());
      auto bv = addressify (b->get_constant ());
      if (av > bv)
	std::swap (av, bv);

      coverage cov;
      cov.add (av.uval (), (bv - av).uval ());
      return value_aset (cov, 0);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes two constants on TOS and constructs an address set that spans
that range.  (The higher address is not considered a part of that
range though.)

)docstring";
    }
  };
}

// add
namespace
{
  struct op_add_aset_cst
    : public op_once_overload <value_aset, value_aset, value_cst>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_aset> a,
	     std::unique_ptr <value_cst> b) override
    {
      auto bv = addressify (b->get_constant ());
      auto ret = a->get_coverage ();
      ret.add (bv.uval (), 1);
      return value_aset {std::move (ret), 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set and a constant on TOS, and adds the constant to
range covered by given address set::

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location address 1 add'
	[0x1, 0x2), [0x10000, 0x10010)
	[0x1, 0x2), [0x10010, 0x1001a)

)docstring";
    }
  };

  struct op_add_aset_aset
    : public op_once_overload <value_aset, value_aset, value_aset>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_aset> a,
	     std::unique_ptr <value_aset> b) override
    {
      auto ret = a->get_coverage ();
      ret.add_all (b->get_coverage ());
      return value_aset {std::move (ret), 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes two address sets and yields an address set that covers both
their ranges::

	$ dwgrep ./tests/aranges.o -e '
		[|D| D entry @AT_location address]
		(|L| L elem (pos == 0) L elem (pos == 1)) add'
	[0x10000, 0x1001a)

)docstring";
    }
  };
}

// sub
namespace
{
  struct op_sub_aset_cst
    : public op_once_overload <value_aset, value_aset, value_cst>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_aset> a,
	     std::unique_ptr <value_cst> b) override
    {
      auto bv = addressify (b->get_constant ());
      auto ret = a->get_coverage ();
      ret.remove (bv.uval (), 1);
      return value_aset {std::move (ret), 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set and a constant on TOS and yields an address set
with a hole poked at the address given by the constant::

	$ dwgrep ./tests/aranges.o -e 'entry @AT_location address 0x10010 sub'
	[0x10000, 0x10010)
	[0x10011, 0x1001a)

)docstring";
    }
  };

  struct op_sub_aset_aset
    : public op_once_overload <value_aset, value_aset, value_aset>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_aset> a,
	     std::unique_ptr <value_aset> b) override
    {
      auto ret = a->get_coverage ();
      ret.remove_all (b->get_coverage ());
      return value_aset {std::move (ret), 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes two address sets *A* and *B*, and yields an address set that
contains all of the *A*'s addresses except those covered by *B*.

)docstring";
    }
  };
}

// length
namespace
{
  struct op_length_aset
    : public op_once_overload <value_cst, value_aset>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_aset> a) override
    {
      uint64_t length = 0;
      for (size_t i = 0; i < a->get_coverage ().size (); ++i)
	length += a->get_coverage ().at (i).length;

      return value_cst {constant {length, &dec_constant_dom}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set on TOS and yields number of addresses covered by
that set::

	$ dwgrep '0 0x10 aset 0x100 0x110 aset add length'
	32

)docstring";
    }
  };
}

// range
namespace
{
  struct op_range_aset
    : public op_yielding_overload <value_aset, value_aset>
  {
    using op_yielding_overload::op_yielding_overload;

    struct producer
      : public value_producer <value_aset>
    {
      std::unique_ptr <value_aset> m_a;
      size_t m_i;

      producer (std::unique_ptr <value_aset> a)
	: m_a {std::move (a)}
	, m_i {0}
      {}

      std::unique_ptr <value_aset>
      next () override
      {
	size_t sz = m_a->get_coverage ().size ();
	if (m_i >= sz)
	  return nullptr;

	coverage ret;
	auto range = m_a->get_coverage ().at (m_i);
	ret.add (range.start, range.length);

	return std::make_unique <value_aset> (ret, m_i++);
      }
    };

    std::unique_ptr <value_producer <value_aset>>
    operate (std::unique_ptr <value_aset> a) override
    {
      return std::make_unique <producer> (std::move (a));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an address set on TOS and yields all continuous ranges of that
address set, presented as individual address sets::

	$ dwgrep '0 0x10 aset 0x100 0x110 aset add range'
	[0, 0x10)
	[0x100, 0x110)

)docstring";
    }
  };
}

// ?contains
namespace
{
  struct pred_containsp_aset_cst
    : public pred_overload <value_aset, value_cst>
  {
    using pred_overload::pred_overload;

    pred_result
    result (value_aset &a, value_cst &b) override
    {
      auto av = addressify (b.get_constant ());
      return pred_result (a.get_coverage ().is_covered (av.uval (), 1));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects an address set and a constant on TOS and holds for those
where the constant is contained within the address set.  Note that the
high end of the address set is not actually considered part of the
address set::

	$ dwgrep 'if (0 10 aset 10 ?contains) then "yes" else "no"'
	no
	$ dwgrep 'if (0 10 aset 9 ?contains) then "yes" else "no"'
	yes

)docstring";
    }
  };

  struct pred_containsp_aset_aset
    : public pred_overload <value_aset, value_aset>
  {
    using pred_overload::pred_overload;

    pred_result
    result (value_aset &a, value_aset &b) override
    {
      // ?contains holds if A contains all of B.
      for (size_t i = 0; i < b.get_coverage ().size (); ++i)
	{
	  auto range = b.get_coverage ().at (i);
	  if (! a.get_coverage ().is_covered (range.start, range.length))
	    return pred_result::no;
	}
      return pred_result::yes;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects two address sets on TOS, *A* and *B*, and holds for those
where all of *B*'s addresses are covered by *A*.

)docstring";
    }
  };
}

// ?overlaps
namespace
{
  struct pred_overlapsp_aset_aset
    : public pred_overload <value_aset, value_aset>
  {
    using pred_overload::pred_overload;

    pred_result
    result (value_aset &a, value_aset &b) override
    {
      for (size_t i = 0; i < b.get_coverage ().size (); ++i)
	{
	  auto range = b.get_coverage ().at (i);
	  if (a.get_coverage ().is_overlap (range.start, range.length))
	    return pred_result::yes;
	}
      return pred_result::no;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects two address sets on TOS, and holds if the two address sets
overlap.

)docstring";
    }
  };
}

// overlap
namespace
{
  struct op_overlap_aset_aset
    : public op_once_overload <value_aset, value_aset, value_aset>
  {
    using op_once_overload::op_once_overload;

    value_aset
    operate (std::unique_ptr <value_aset> a,
	     std::unique_ptr <value_aset> b) override
    {
      coverage ret;
      for (size_t i = 0; i < b->get_coverage ().size (); ++i)
	{
	  auto range = b->get_coverage ().at (i);
	  auto cov = a->get_coverage ().intersect (range.start, range.length);
	  ret.add_all (cov);
	}
      return value_aset {std::move (ret), 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes two address sets on TOS, and yields a (possibly empty) address
set that covers those addresses that both of the address sets cover.

)docstring";
    }
  };
}

// ?empty
namespace
{
  struct pred_emptyp_aset
    : public pred_overload <value_aset>
  {
    using pred_overload::pred_overload;

    pred_result
    result (value_aset &a) override
    {
      return pred_result (a.get_coverage ().empty ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects an address set on TOS and holds if it is empty (contains no
addresses).  Could be written as ``!(elem)``.

)docstring";
    }
  };
}


// ?haschildren
namespace
{
  struct pred_haschildrenp_die
    : public pred_overload <value_die>
  {
    using pred_overload::pred_overload;

    pred_result
    result (value_die &a) override
    {
      return pred_result (dwarf_haschildren (&a.get_die ()));
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a DIE on TOS and holds if that DIE might have children,
i.e. it is the same as ``abbrev ?haschildren``.  Note that even DIE's
for which ?haschildren holds may not actually have children, because
the actual children chain in the Dwarf file is immediately terminated.
To determine whether there are actually any children, use
``?(child)``::

	$ dwgrep ./tests/haschildren_childless -e 'entry (offset == 0x2d) abbrev "%s"'
	[2] offset:0x13, children:yes, tag:subprogram

	$ dwgrep ./tests/haschildren_childless -e '
		entry (offset == 0x2d)
		if child then "yes" else "no" "%s: %s"'
	[2d] subprogram: no

)docstring";
    }
  };
}

// version
namespace
{
  struct op_version_cu
    : public op_once_overload <value_cst, value_cu>
  {
    using op_once_overload::op_once_overload;

    value_cst
    operate (std::unique_ptr <value_cu> a) override
    {
      Dwarf_CU &cu = a->get_cu ();
      Dwarf_Die cudie;
      Dwarf_Half version;
      if (dwarf_cu_die (&cu, &cudie, &version, nullptr,
			nullptr, nullptr, nullptr, nullptr) == nullptr)
	throw_libdw ();

      return value_cst {constant {version, &dec_constant_dom}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yields a constant corresponding to version of
Dwarf standard according to which this CU has been written.

)docstring";
    }
  };
}

// name
namespace
{
  struct op_name_dwarf
    : public op_once_overload <value_str, value_dwarf>
  {
    using op_once_overload::op_once_overload;

    value_str
    operate (std::unique_ptr <value_dwarf> a) override
    {
      return value_str {std::string {a->get_fn ()}, 0};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Take a Dwarf on TOS and yield its filename.  Note that it may not be
possible to use the reported name to reopen the Dwarf.  The Dwarf may
have been built in place in memory, or the file may have been deleted
or replaced since the Dwarf was opened.

)docstring";
    }
  };

  struct op_name_die
    : public op_overload <value_str, value_die>
  {
    using op_overload::op_overload;

    std::unique_ptr <value_str>
    operate (std::unique_ptr <value_die> a) override
    {
      if (a->is_cooked ())
	{
	  // On cooked DIE's, `name` integrates.
	  const char *name = dwarf_diename (&a->get_die ());
	  if (name != nullptr)
	    return std::make_unique <value_str> (name, 0);
	  else
	    return nullptr;
	}
      // Unfortunately there's no non-integrating dwarf_diename
      // counterpart.
      else if (dwarf_hasattr (&a->get_die (), DW_AT_name))
	{
	  Dwarf_Attribute attr = dwpp_attr (a->get_die (), DW_AT_name);
	  return std::make_unique <value_str> (dwpp_formstring (attr), 0);
	}
      else
	return nullptr;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Equivalent to ``@AT_name``.

)docstring";
    }
  };
}

// raw
namespace
{
  struct op_raw_dwarf
    : public op_once_overload <value_dwarf, value_dwarf>
  {
    using op_once_overload::op_once_overload;

    value_dwarf
    operate (std::unique_ptr <value_dwarf> a) override
    {
      return value_dwarf {a->get_fn (), a->get_dwctx (), 0, doneness::raw};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a Dwarf on TOS and yields a raw version thereof.

|RawCookeImmutable|

.. |RawCookeImmutable| replace::

   The words ``raw`` and ``cooked`` create a new value, they do not
   change a state of an existing one.  The underlying bits that
   constitute the Dwarf data themselves are shared, but those, too,
   are immutable.

In particular, take a look at the following example::

	$ dwgrep ./tests/a1.out -e '
		(|D| D raw D (|R C| [C entry] length [R entry] length))'
	---
	13
	11

Had ``raw`` changed the value that it's applied to, the next reference
to *D* would yield a raw Dwarf.  But raw yields a new value and *D* is
kept intact (despite the sharing of underlying bits, as mentioned).

)docstring";
    }
  };

  struct op_raw_cu
    : public op_once_overload <value_cu, value_cu>
  {
    using op_once_overload::op_once_overload;

    value_cu
    operate (std::unique_ptr <value_cu> a) override
    {
      return value_cu {a->get_dwctx (), a->get_cu (), a->get_offset (),
		       0, doneness::raw};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yields a raw version thereof.

|RawCookeImmutable|

)docstring";
    }
  };

  struct op_raw_die
    : public op_once_overload <value_die, value_die>
  {
    using op_once_overload::op_once_overload;

    value_die
    operate (std::unique_ptr <value_die> a) override
    {
      return value_die {a->get_dwctx (), a->get_die (), 0, doneness::raw};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields a raw version thereof.

|RawCookeImmutable|

)docstring";
    }
  };

  struct op_raw_attr
    : public op_once_overload <value_attr, value_attr>
  {
    using op_once_overload::op_once_overload;

    value_attr
    operate (std::unique_ptr <value_attr> a) override
    {
      return value_attr {a->get_dwctx (), a->get_attr (), a->get_die (),
			 0, doneness::raw};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an attribute on TOS and yields a raw version thereof.

|RawCookeImmutable|

)docstring";
    }
  };
}

// cooked
namespace
{
  struct op_cooked_dwarf
    : public op_once_overload <value_dwarf, value_dwarf>
  {
    using op_once_overload::op_once_overload;

    value_dwarf
    operate (std::unique_ptr <value_dwarf> a) override
    {
      return value_dwarf {a->get_fn (), a->get_dwctx (), 0, doneness::cooked};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a Dwarf on TOS and yields a cooked version thereof.

|RawCookeImmutable|

)docstring";
    }
  };

  struct op_cooked_cu
    : public op_once_overload <value_cu, value_cu>
  {
    using op_once_overload::op_once_overload;

    value_cu
    operate (std::unique_ptr <value_cu> a) override
    {
      return value_cu {a->get_dwctx (), a->get_cu (), a->get_offset (),
		       0, doneness::cooked};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a CU on TOS and yields a cooked version thereof.

|RawCookeImmutable|

)docstring";
    }
  };

  struct op_cooked_die
    : public op_once_overload <value_die, value_die>
  {
    using op_once_overload::op_once_overload;

    value_die
    operate (std::unique_ptr <value_die> a) override
    {
      return value_die {a->get_dwctx (), a->get_die (), 0, doneness::cooked};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields a cooked version thereof.

|RawCookeImmutable|

)docstring";
    }
  };

  struct op_cooked_attr
    : public op_once_overload <value_attr, value_attr>
  {
    using op_once_overload::op_once_overload;

    value_attr
    operate (std::unique_ptr <value_attr> a) override
    {
      return value_attr {a->get_dwctx (), a->get_attr (), a->get_die (),
			 0, doneness::cooked};
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes an attribute on TOS and yields a cooked version thereof.

|RawCookeImmutable|

)docstring";
    }
  };
}

// @AT_*
namespace
{
  bool
  find_attribute (Dwarf_Die a, int atname, doneness d, Dwarf_Attribute *ret)
  {
    if (dwarf_hasattr (&a, atname))
      {
	if (ret != nullptr)
	  *ret = dwpp_attr (a, atname);
	return true;
      }
    else if (d == doneness::cooked && attr_should_be_integrated (atname))
      {
	auto recursively_find_at = [&] (int atname2)
	  {
	    if (dwarf_hasattr (&a, atname2))
	      {
		Dwarf_Attribute at = dwpp_attr (a, atname2);
		return find_attribute (dwpp_formref_die (at), atname, d, ret);
	      }
	    else
	      return false;
	  };

	return recursively_find_at (DW_AT_specification)
	  || recursively_find_at (DW_AT_abstract_origin);
      }
    else
      return false;
  }

  class op_atval_die
    : public op_yielding_overload <value, value_die>
  {
    int m_atname;

  public:
    op_atval_die (std::shared_ptr <op> upstream, int atname)
      : op_yielding_overload {upstream}
      , m_atname {atname}
    {}

    std::unique_ptr <value_producer <value>>
    operate (std::unique_ptr <value_die> a)
    {
      Dwarf_Attribute attr;
      if (! find_attribute (a->get_die (), m_atname,
			    a->get_doneness (), &attr))
	return nullptr;

      return at_value (a->get_dwctx (), a->get_die (), attr);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Takes a DIE on TOS and yields value of attribute indicated by the
word.  Syntactic sugar for ``(attribute ?(label == AT_*) cooked
value)``::

	$ dwgrep ./tests/nullptr.o -e '
		entry (|A| "%( A @AT_name %): %( A @AT_declaration %)")'
	foo: true

)docstring";
    }
  };
}

// ?AT_*
namespace
{
  struct pred_atname_die
    : public pred_overload <value_die>
  {
    unsigned m_atname;

    pred_atname_die (unsigned atname)
      : m_atname {atname}
    {}

    pred_result
    result (value_die &a) override
    {
      return find_attribute (a.get_die (), m_atname,
			     a.get_doneness (), nullptr)
	? pred_result::yes : pred_result::no;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a DIE on TOS and holds if that DIE has the attribute
indicated by this word.  Note that for cooked DIE's, this word
integrates attributes from abstract origins and specifications,
according to the same rules as ``attribute`` uses.

For example, in the following, a DIE 0x6e reports that it has an
``DW_AT_name`` attribute even if it in fact does not.  This is due to
``DW_AT_specification`` bringing this in::

	$ dwgrep ./tests/nullptr.o -e 'entry (offset == 0x6e) ?AT_name'
	[6e]	subprogram
		specification (ref4)	[3f];
		inline (data1)	DW_INL_declared_inlined;
		object_pointer (ref4)	[7c];
		sibling (ref4)	[8b];

	$ dwgrep ./tests/nullptr.o -e 'entry (offset == 0x3f)'
	[3f]	subprogram
		external (flag_present)	true;
		name (string)	foo;
		decl_file (data1)	tests/nullptr.cc;
		decl_line (data1)	3;
		declaration (flag_present)	true;
		object_pointer (ref4)	[4a];

)docstring";
    }
  };

  struct pred_atname_attr
    : public pred_overload <value_attr>
  {
    unsigned m_atname;

    pred_atname_attr (unsigned atname)
      : m_atname {atname}
    {}

    pred_result
    result (value_attr &a) override
    {
      return pred_result (dwarf_whatattr (&a.get_attr ()) == m_atname);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects an attribute on TOS and holds if name of that attribute is
the same as indicated by this word::

	$ dwgrep ./tests/nullptr.o -e 'entry (offset == 0x6e) attribute ?AT_name'
	name (string)	foo;

)docstring";
    }
  };

  struct pred_atname_cst
    : public pred_overload <value_cst>
  {
    constant m_const;

    pred_atname_cst (unsigned atname)
      : m_const {atname, &dw_attr_dom ()}
    {}

    pred_result
    result (value_cst &a) override
    {
      return pred_result (m_const == a.get_constant ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a constant on TOS and holds if it's the same constant as
indicated by this word::

	$ dwgrep ./tests/nullptr.o -e '
		entry (offset == 0x4a) abbrev attribute label'
	DW_AT_type
	DW_AT_artificial

	$ dwgrep ./tests/nullptr.o -e '
		entry (offset == 0x4a) abbrev attribute label ?AT_type'
	DW_AT_type

)docstring";
    }
  };
}

// ?TAG_*
namespace
{
  struct pred_tag_die
    : public pred_overload <value_die>
  {
    int m_tag;

    pred_tag_die (int tag)
      : m_tag {tag}
    {}

    pred_result
    result (value_die &a) override
    {
      return pred_result (dwarf_tag (&a.get_die ()) == m_tag);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a DIE on TOS and holds if its tag is the same as this word
indicates::

	$ dwgrep ./tests/nullptr.o -e 'entry ?TAG_unspecified_type'
	[69]	unspecified_type
		name (strp)	decltype(nullptr);

)docstring";
    }
  };

  struct pred_tag_cst
    : public pred_overload <value_cst>
  {
    constant m_const;

    pred_tag_cst (int tag)
      : m_const {(unsigned) tag, &dw_tag_dom ()}
    {}

    pred_result
    result (value_cst &a) override
    {
      return pred_result (m_const == a.get_constant ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a constant on TOS and holds if it's the same constant as
indicated by this word::

	$ dwgrep 'DW_TAG_formal_parameter ?TAG_formal_parameter'
	DW_TAG_formal_parameter

	$ dwgrep -c 'DW_TAG_formal_parameter value ?TAG_formal_parameter'
	0

)docstring";
    }
  };
}

// ?FORM_*
namespace
{
  struct pred_form_attr
    : public pred_overload <value_attr>
  {
    unsigned m_form;

    pred_form_attr (unsigned form)
      : m_form {form}
    {}

    pred_result
    result (value_attr &a) override
    {
      return pred_result (dwarf_whatform (&a.get_attr ()) == m_form);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects an attribute on TOS and holds if the form of that attribute
is the same as indicated by this word::

	$ dwgrep ./tests/nullptr.o -e 'entry ?(attribute ?FORM_data2)'
	[e3]	formal_parameter
		abstract_origin (ref4)	[a5];
		const_value (data2)	65535;

)docstring";
    }
  };

  struct pred_form_cst
    : public pred_overload <value_cst>
  {
    constant m_const;

    pred_form_cst (unsigned form)
      : m_const {form, &dw_form_dom ()}
    {}

    pred_result
    result (value_cst &a) override
    {
      return pred_result (m_const == a.get_constant ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a constant on TOS and holds if it's the same as indicated by
this word.

)docstring";
    }
  };
}

// ?OP_*
namespace
{
  struct pred_op_loclist_elem
    : public pred_overload <value_loclist_elem>
  {
    unsigned m_op;

    pred_op_loclist_elem (unsigned op)
      : m_op {op}
    {}

    pred_result
    result (value_loclist_elem &a) override
    {
      for (size_t i = 0; i < a.get_exprlen (); ++i)
	if (a.get_expr ()[i].atom == m_op)
	  return pred_result::yes;
      return pred_result::no;
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a location expression on TOS and holds if it contains an
instruction with an opcode indicated by this word::

	$ dwgrep ./tests/bitcount.o -e 'entry @AT_location ?OP_and'
	0x10017..0x1001a:[0:breg5<0>, 2:breg1<0>, 4:and, 5:stack_value]

)docstring";
    }
  };

  struct pred_op_loclist_op
    : public pred_overload <value_loclist_op>
  {
    unsigned m_op;

    pred_op_loclist_op (unsigned op)
      : m_op {op}
    {}

    pred_result
    result (value_loclist_op &a) override
    {
      return pred_result (a.get_dwop ()->atom == m_op);
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a location expression instruction on TOS and holds if its
opcode is the same as indicated by this word::

	$ dwgrep ./tests/bitcount.o -e 'entry @AT_location elem ?OP_and'
	4:and

)docstring";
    }
  };

  struct pred_op_cst
    : public pred_overload <value_cst>
  {
    constant m_const;

    pred_op_cst (unsigned form)
      : m_const {form, &dw_locexpr_opcode_dom ()}
    {}

    pred_result
    result (value_cst &a) override
    {
      return pred_result (m_const == a.get_constant ());
    }

    static std::string
    docstring ()
    {
      return
R"docstring(

Inspects a constant on TOS and holds if it is the same as indicated by
this word.

)docstring";
    }
  };
}

std::unique_ptr <vocabulary>
dwgrep_vocabulary_dw ()
{
  auto ret = std::make_unique <vocabulary> ();
  vocabulary &voc = *ret;

  add_builtin_type_constant <value_dwarf> (voc);
  add_builtin_type_constant <value_cu> (voc);
  add_builtin_type_constant <value_die> (voc);
  add_builtin_type_constant <value_attr> (voc);
  add_builtin_type_constant <value_abbrev_unit> (voc);
  add_builtin_type_constant <value_abbrev> (voc);
  add_builtin_type_constant <value_abbrev_attr> (voc);
  add_builtin_type_constant <value_aset> (voc);
  add_builtin_type_constant <value_loclist_elem> (voc);
  add_builtin_type_constant <value_loclist_op> (voc);

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_dwopen_str> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("dwopen", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_unit_dwarf> ();
    t->add_op_overload <op_unit_die> ();
    t->add_op_overload <op_unit_attr> ();
    // xxx rawattr

    voc.add (std::make_shared <overloaded_op_builtin> ("unit", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_entry_dwarf> ();
    t->add_op_overload <op_entry_cu> ();
    t->add_op_overload <op_entry_abbrev_unit> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("entry", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_attribute_die> ();
    t->add_op_overload <op_attribute_abbrev> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("attribute", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_pred_overload <pred_rootp_die> ();

    voc.add (std::make_shared <overloaded_pred_builtin> ("?root", t, true));
    voc.add (std::make_shared <overloaded_pred_builtin> ("!root", t, false));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_root_cu> ();
    t->add_op_overload <op_root_die> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("root", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_child_die> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("child", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_elem_loclist_elem> ();
    t->add_op_overload <op_elem_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("elem", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_relem_loclist_elem> ();
    t->add_op_overload <op_relem_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("relem", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_value_attr> ();
    // xxx raw
    t->add_op_overload <op_value_loclist_op> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("value", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_offset_cu> ();
    t->add_op_overload <op_offset_die> ();
    t->add_op_overload <op_offset_abbrev_unit> ();
    t->add_op_overload <op_offset_abbrev> ();
    t->add_op_overload <op_offset_abbrev_attr> ();
    t->add_op_overload <op_offset_loclist_op> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("offset", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_address_die> ();
    t->add_op_overload <op_address_attr> ();
    t->add_op_overload <op_address_loclist_elem> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("address", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_label_die> ();
    t->add_op_overload <op_label_attr> ();
    t->add_op_overload <op_label_abbrev> ();
    t->add_op_overload <op_label_abbrev_attr> ();
    t->add_op_overload <op_label_loclist_op> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("label", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_form_attr> ();
    t->add_op_overload <op_form_abbrev_attr> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("form", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_parent_die> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("parent", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_low_die> ();
    t->add_op_overload <op_low_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("low", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_high_die> ();
    t->add_op_overload <op_high_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("high", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_aset_cst_cst> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("aset", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_add_aset_cst> ();
    t->add_op_overload <op_add_aset_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("add", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_sub_aset_cst> ();
    t->add_op_overload <op_sub_aset_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("sub", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_length_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("length", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_range_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("range", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_pred_overload <pred_containsp_aset_cst> ();
    t->add_pred_overload <pred_containsp_aset_aset> ();

    voc.add
      (std::make_shared <overloaded_pred_builtin> ("?contains", t, true));
    voc.add
      (std::make_shared <overloaded_pred_builtin> ("!contains", t, false));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_pred_overload <pred_overlapsp_aset_aset> ();

    voc.add
      (std::make_shared <overloaded_pred_builtin> ("?overlaps", t, true));
    voc.add
      (std::make_shared <overloaded_pred_builtin> ("!overlaps", t, false));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_overlap_aset_aset> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("overlap", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_pred_overload <pred_emptyp_aset> ();

    voc.add (std::make_shared <overloaded_pred_builtin> ("?empty", t, true));
    voc.add (std::make_shared <overloaded_pred_builtin> ("!empty", t, false));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_abbrev_dwarf> ();
    t->add_op_overload <op_abbrev_cu> ();
    t->add_op_overload <op_abbrev_die> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("abbrev", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_pred_overload <pred_haschildrenp_die> ();
    t->add_pred_overload <pred_haschildrenp_abbrev> ();

    voc.add (std::make_shared
	     <overloaded_pred_builtin> ("?haschildren", t, true));
    voc.add (std::make_shared
	     <overloaded_pred_builtin> ("!haschildren", t, false));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_code_abbrev> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("code", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_version_cu> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("version", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_name_dwarf> ();
    t->add_op_overload <op_name_die> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("name", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_raw_dwarf> ();
    t->add_op_overload <op_raw_cu> ();
    t->add_op_overload <op_raw_die> ();
    t->add_op_overload <op_raw_attr> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("raw", t));
  }

  {
    auto t = std::make_shared <overload_tab> ();

    t->add_op_overload <op_cooked_dwarf> ();
    t->add_op_overload <op_cooked_cu> ();
    t->add_op_overload <op_cooked_die> ();
    t->add_op_overload <op_cooked_attr> ();

    voc.add (std::make_shared <overloaded_op_builtin> ("cooked", t));
  }

  auto add_dw_at = [&voc] (unsigned code,
			   char const *qname, char const *bname,
			   char const *atname,
			   char const *lqname, char const *lbname,
			   char const *latname)
    {
      // ?AT_* etc.
      {
	auto t = std::make_shared <overload_tab> ();

	t->add_pred_overload <pred_atname_die> (code);
	t->add_pred_overload <pred_atname_attr> (code);
	t->add_pred_overload <pred_atname_abbrev> (code);
	t->add_pred_overload <pred_atname_abbrev_attr> (code);
	t->add_pred_overload <pred_atname_cst> (code);

	voc.add (std::make_shared <overloaded_pred_builtin> (qname, t, true));
	voc.add (std::make_shared <overloaded_pred_builtin> (bname, t, false));
	voc.add (std::make_shared <overloaded_pred_builtin> (lqname, t, true));
	voc.add (std::make_shared <overloaded_pred_builtin> (lbname, t, false));
      }

      // @AT_* etc.
      {
	auto t = std::make_shared <overload_tab> ();

	t->add_op_overload <op_atval_die> (code);
	// xxx raw shouldn't interpret values

	voc.add (std::make_shared <overloaded_op_builtin> (atname, t));
	voc.add (std::make_shared <overloaded_op_builtin> (latname, t));
      }

      // DW_AT_*
      add_builtin_constant (voc, constant (code, &dw_attr_dom ()), lqname + 1);
    };

#define ONE_KNOWN_DW_AT_DESC(NAME, CODE, DESC) ONE_KNOWN_DW_AT(NAME, CODE)
#define ONE_KNOWN_DW_AT(NAME, CODE)					\
  add_dw_at (CODE, "?AT_" #NAME, "!AT_" #NAME, "@AT_" #NAME,		\
	     "?" #CODE, "!" #CODE, "@" #CODE);
  ALL_KNOWN_DW_AT;
#undef ONE_KNOWN_DW_AT
#undef ONE_KNOWN_DW_AT_DESC

  auto add_dw_tag = [&voc] (int code,
			    char const *qname, char const *bname,
			    char const *lqname, char const *lbname)
    {
      auto t = std::make_shared <overload_tab> ();

      t->add_pred_overload <pred_tag_die> (code);
      t->add_pred_overload <pred_tag_abbrev> (code);
      t->add_pred_overload <pred_tag_cst> (code);

      voc.add (std::make_shared <overloaded_pred_builtin> (qname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (bname, t, false));
      voc.add (std::make_shared <overloaded_pred_builtin> (lqname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (lbname, t, false));

      add_builtin_constant (voc, constant (code, &dw_tag_dom ()), lqname + 1);
    };

#define ONE_KNOWN_DW_TAG(NAME, CODE)					\
  add_dw_tag (CODE, "?TAG_" #NAME, "!TAG_" #NAME, "?" #CODE, "!" #CODE);
  ALL_KNOWN_DW_TAG;
#undef ONE_KNOWN_DW_TAG

  auto add_dw_form = [&voc] (unsigned code,
			     char const *qname, char const *bname,
			     char const *lqname, char const *lbname)
    {
      auto t = std::make_shared <overload_tab> ();

      t->add_pred_overload <pred_form_attr> (code);
      t->add_pred_overload <pred_form_abbrev_attr> (code);
      t->add_pred_overload <pred_form_cst> (code);

      voc.add (std::make_shared <overloaded_pred_builtin> (qname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (bname, t, false));
      voc.add (std::make_shared <overloaded_pred_builtin> (lqname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (lbname, t, false));

      add_builtin_constant (voc, constant (code, &dw_form_dom ()), lqname + 1);
    };

#define ONE_KNOWN_DW_FORM_DESC(NAME, CODE, DESC) ONE_KNOWN_DW_FORM (NAME, CODE)
#define ONE_KNOWN_DW_FORM(NAME, CODE)					\
  add_dw_form (CODE, "?FORM_" #NAME, "!FORM_" #NAME, "?" #CODE, "!" #CODE);
  ALL_KNOWN_DW_FORM;
#undef ONE_KNOWN_DW_FORM
#undef ONE_KNOWN_DW_FORM_DESC

  auto add_dw_op = [&voc] (unsigned code,
			   char const *qname, char const *bname,
			   char const *lqname, char const *lbname)
    {
      auto t = std::make_shared <overload_tab> ();

      t->add_pred_overload <pred_op_loclist_elem> (code);
      t->add_pred_overload <pred_op_loclist_op> (code);
      t->add_pred_overload <pred_op_cst> (code);

      voc.add (std::make_shared <overloaded_pred_builtin> (qname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (bname, t, false));
      voc.add (std::make_shared <overloaded_pred_builtin> (lqname, t, true));
      voc.add (std::make_shared <overloaded_pred_builtin> (lbname, t, false));

      add_builtin_constant (voc, constant (code, &dw_locexpr_opcode_dom ()),
			    lqname + 1);
    };

#define ONE_KNOWN_DW_OP_DESC(NAME, CODE, DESC) ONE_KNOWN_DW_OP (NAME, CODE)
#define ONE_KNOWN_DW_OP(NAME, CODE)					\
  add_dw_op (CODE, "?OP_" #NAME, "!OP_" #NAME, "?" #CODE, "!" #CODE);
  ALL_KNOWN_DW_OP;
#undef ONE_KNOWN_DW_OP
#undef ONE_KNOWN_DW_OP_DESC

#define ONE_KNOWN_DW_LANG_DESC(NAME, CODE, DESC)			\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_lang_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_LANG;
#undef ONE_KNOWN_DW_LANG_DESC

#define ONE_KNOWN_DW_MACINFO(NAME, CODE)				\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_macinfo_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_MACINFO;
#undef ONE_KNOWN_DW_MACINFO

#define ONE_KNOWN_DW_MACRO_GNU(NAME, CODE)				\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_macro_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_MACRO_GNU;
#undef ONE_KNOWN_DW_MACRO_GNU

#define ONE_KNOWN_DW_INL(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_inline_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_INL;
#undef ONE_KNOWN_DW_INL

#define ONE_KNOWN_DW_ATE(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_encoding_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_ATE;
#undef ONE_KNOWN_DW_ATE

#define ONE_KNOWN_DW_ACCESS(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_access_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_ACCESS;
#undef ONE_KNOWN_DW_ACCESS

#define ONE_KNOWN_DW_VIS(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_visibility_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_VIS;
#undef ONE_KNOWN_DW_VIS

#define ONE_KNOWN_DW_VIRTUALITY(NAME, CODE)				\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_virtuality_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_VIRTUALITY;
#undef ONE_KNOWN_DW_VIRTUALITY

#define ONE_KNOWN_DW_ID(NAME, CODE)					\
  {									\
    add_builtin_constant (voc,						\
			  constant (CODE, &dw_identifier_case_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_ID;
#undef ONE_KNOWN_DW_ID

#define ONE_KNOWN_DW_CC(NAME, CODE)					\
  {									\
    add_builtin_constant (voc,						\
			  constant (CODE, &dw_calling_convention_dom ()), \
			  #CODE);					\
  }
  ALL_KNOWN_DW_CC;
#undef ONE_KNOWN_DW_CC

#define ONE_KNOWN_DW_ORD(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_ordering_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_ORD;
#undef ONE_KNOWN_DW_ORD

#define ONE_KNOWN_DW_DSC(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_discr_list_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_DSC;
#undef ONE_KNOWN_DW_DSC

#define ONE_KNOWN_DW_DS(NAME, CODE)					\
  {									\
    add_builtin_constant (voc,						\
			  constant (CODE, &dw_decimal_sign_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_DS;
#undef ONE_KNOWN_DW_DS

  add_builtin_constant (voc, constant (DW_ADDR_none, &dw_address_class_dom ()),
			"DW_ADDR_none");

#define ONE_KNOWN_DW_END(NAME, CODE)					\
  {									\
    add_builtin_constant (voc, constant (CODE, &dw_endianity_dom ()), #CODE); \
  }
  ALL_KNOWN_DW_END;
#undef ONE_KNOWN_DW_END

  return ret;
}
