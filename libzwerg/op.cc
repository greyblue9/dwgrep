/*
   Copyright (C) 2017 Petr Machata
   Copyright (C) 2014, 2015 Red Hat, Inc.
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

#include <iostream>
#include <sstream>
#include <memory>
#include <set>
#include <algorithm>

#include "op.hh"
#include "builtin-closure.hh"
#include "overload.hh"
#include "value-closure.hh"
#include "value-cst.hh"
#include "value-seq.hh"
#include "value-str.hh"

namespace
{
  void
  debug_stack (stack const &stk)
  {
    {
      std::vector <std::reference_wrapper <value const>> stack;
      for (size_t i = 0, n = stk.size (); i < n; ++i)
	stack.push_back (stk.get (i));

      std::cerr << "<";
      std::for_each (stack.rbegin (), stack.rend (),
		     [&stk] (std::reference_wrapper <value const> v) {
		       v.get ().show ((std::cerr << ' '));
		     });
      std::cerr << " > (";
    }

    std::shared_ptr <frame> frame = stk.nth_frame (0);
    while (frame != nullptr)
      {
	std::cerr << frame << ':' << frame.use_count ();
	std::cerr << "{";
	for (auto const &v: frame->m_values)
	  if (v == nullptr)
	    std::cerr << " (unbound)";
	  else
	    v->show (std::cerr << ' ');
	std::cerr << " }  ";

	frame = frame->m_parent;
      }
    std::cerr << ")\n";
  }
}

stack::uptr
op_origin::next ()
{
  return std::move (m_stk);
}

std::string
op_origin::name () const
{
  return "origin";
}

void
op_origin::set_next (stack::uptr s)
{
  assert (m_stk == nullptr);

  // set_next should have been preceded with a reset() call that
  // should have percolated all the way here.
  assert (m_reset);
  m_reset = false;

  m_stk = std::move (s);
}

void
op_origin::reset ()
{
  m_stk = nullptr;
  m_reset = true;
}


stack::uptr
op_nop::next ()
{
  return m_upstream->next ();
}

std::string
op_nop::name () const
{
  return "nop";
}


stack::uptr
op_assert::next ()
{
  while (auto stk = m_upstream->next ())
    if (m_pred->result (*stk) == pred_result::yes)
      return stk;
  return nullptr;
}

std::string
op_assert::name () const
{
  return std::string ("assert<") + m_pred->name () + ">";
}


void
stringer_origin::set_next (stack::uptr s)
{
  assert (m_stk == nullptr);

  // set_next should have been preceded with a reset() call that
  // should have percolated all the way here.
  assert (m_reset);
  m_reset = false;

  m_stk = std::move (s);
}

std::pair <stack::uptr, std::string>
stringer_origin::next ()
{
  return std::make_pair (std::move (m_stk), "");
}

void
stringer_origin::reset ()
{
  m_stk = nullptr;
  m_reset = true;
}

std::pair <stack::uptr, std::string>
stringer_lit::next ()
{
  auto up = m_upstream->next ();
  if (up.first == nullptr)
    return std::make_pair (nullptr, "");
  up.second = m_str + up.second;
  return up;
}

void
stringer_lit::reset ()
{
  m_upstream->reset ();
}

std::pair <stack::uptr, std::string>
stringer_op::next ()
{
  while (true)
    {
      if (! m_have)
	{
	  auto up = m_upstream->next ();
	  if (up.first == nullptr)
	    return std::make_pair (nullptr, "");

	  m_op->reset ();
	  m_origin->set_next (std::move (up.first));
	  m_str = up.second;

	  m_have = true;
	}

      if (auto stk = m_op->next ())
	{
	  std::stringstream ss;
	  (stk->pop ())->show (ss);
	  return std::make_pair (std::move (stk), ss.str () + m_str);
	}

      m_have = false;
    }
}

void
stringer_op::reset ()
{
  m_have = false;
  m_op->reset ();
  m_upstream->reset ();
}


op_format::op_format (std::shared_ptr <op> upstream,
		      std::shared_ptr <stringer_origin> origin,
		      std::shared_ptr <stringer> stringer)
  : inner_op {upstream}
  , m_origin {origin}
  , m_stringer {stringer}
  , m_pos {0}
{}

stack::uptr
op_format::next ()
{
  while (true)
    {
      auto stk = m_stringer->next ();
      if (stk.first != nullptr)
	{
	  stk.first->push (std::make_unique <value_str>
			   (std::move (stk.second), m_pos++));
	  return std::move (stk.first);
	}

      if (auto stk = m_upstream->next ())
	{
	  reset_me ();
	  m_origin->set_next (std::move (stk));
	}
      else
	return nullptr;
    }
}

void
op_format::reset_me ()
{
  m_stringer->reset ();
  m_pos = 0;
}

void
op_format::reset ()
{
  reset_me ();
  inner_op::reset ();
}

std::string
op_format::name () const
{
  return "format";
}


stack::uptr
op_const::next ()
{
  if (auto stk = m_upstream->next ())
    {
      stk->push (m_value->clone ());
      return stk;
    }
  return nullptr;
}

std::string
op_const::name () const
{
  std::stringstream ss;
  ss << "const<";
  m_value->show (ss);
  ss << ">";
  return ss.str ();
}


stack::uptr
op_tine::next ()
{
  if (*m_done)
    return nullptr;

  if (std::all_of (m_file->begin (), m_file->end (),
		   [] (stack::uptr const &ptr) { return ptr == nullptr; }))
    {
      if (auto stk = m_upstream->next ())
	for (auto &ptr: *m_file)
	  ptr = std::make_unique <stack> (*stk);
      else
	{
	  *m_done = true;
	  return nullptr;
	}
    }

  return std::move ((*m_file)[m_branch_id]);
}

void
op_tine::reset ()
{
  for (auto &stk: *m_file)
    stk = nullptr;
  inner_op::reset ();
}

std::string
op_tine::name () const
{
  return "tine";
}


stack::uptr
op_merge::next ()
{
  if (*m_done)
    return nullptr;

  while (! *m_done)
    {
      if (auto ret = (*m_it)->next ())
	return ret;
      if (++m_it == m_ops.end ())
	m_it = m_ops.begin ();
    }

  return nullptr;
}

void
op_merge::reset ()
{
  *m_done = false;
  m_it = m_ops.begin ();
  for (auto op: m_ops)
    op->reset ();
}

std::string
op_merge::name () const
{
  return "merge";
}


void
op_or::reset_me ()
{
  m_branch_it = m_branches.end ();
  for (auto const &branch: m_branches)
    branch.second->reset ();
}

void
op_or::reset ()
{
  reset_me ();
  inner_op::reset ();
}

stack::uptr
op_or::next ()
{
  while (true)
    {
      while (m_branch_it == m_branches.end ())
	{
	  if (auto stk = m_upstream->next ())
	    for (m_branch_it = m_branches.begin ();
		 m_branch_it != m_branches.end (); ++m_branch_it)
	      {
		m_branch_it->second->reset ();
		m_branch_it->first->set_next (std::make_unique <stack> (*stk));
		if (auto stk2 = m_branch_it->second->next ())
		  return stk2;
	      }
	  else
	    return nullptr;
	}

      if (auto stk2 = m_branch_it->second->next ())
	return stk2;

      reset_me ();
    }
}

std::string
op_or::name () const
{
  std::stringstream ss;
  ss << "or<";
  bool sep = false;
  for (auto const &branch: m_branches)
    {
      if (sep)
	ss << " || ";
      sep = true;
      ss << branch.second->name ();
    }
  ss << ">";
  return ss.str ();
}


stack::uptr
op_capture::next ()
{
  if (auto stk = m_upstream->next ())
    {
      m_op->reset ();
      m_origin->set_next (std::make_unique <stack> (*stk));

      value_seq::seq_t vv;
      while (auto stk2 = m_op->next ())
	vv.push_back (stk2->pop ());

      stk->push (std::make_unique <value_seq> (std::move (vv), 0));
      return stk;
    }

  return nullptr;
}

void
op_capture::reset ()
{
  m_op->reset ();
  inner_op::reset ();
}

std::string
op_capture::name () const
{
  return std::string ("capture<") + m_op->name () + ">";
}


op_tr_closure::op_tr_closure (std::shared_ptr <op> upstream,
			      std::shared_ptr <op_origin> origin,
			      std::shared_ptr <op> op,
			      op_tr_closure_kind k)
  : inner_op {upstream}
  , m_origin {origin}
  , m_op {op}
  , m_is_plus {k == op_tr_closure_kind::plus}
  , m_op_drained {true}
{}

std::unique_ptr <stack>
op_tr_closure::yield_and_cache (std::shared_ptr <stack> stk)
{
  if (m_seen.insert (stk).second)
    {
      m_stks.push_back (stk);
      return std::make_unique <stack> (*stk);
    }
  else
    return nullptr;
}

stack::uptr
op_tr_closure::next_from_upstream ()
{
  // When we get a new stack from upstream, that provides a fresh
  // context, and we need to forget what we've seen so far.
  // E.g. consider the following expression:
  //
  //     $ 'entry root dup child* ?eq'
  //
  // We should see as many root-root matches as there are entries.
  // But if we fail to clear the seen-cache, we only see one.

  m_seen.clear ();
  return m_upstream->next ();
}

stack::uptr
op_tr_closure::next_from_op ()
{
  if (m_op_drained)
    return nullptr;
  if (auto ret = m_op->next ())
    return ret;
  m_op_drained = true;
  return nullptr;
}

bool
op_tr_closure::send_to_op (std::unique_ptr <stack> stk)
{
  if (stk == nullptr)
    return false;

  m_op->reset ();
  m_origin->set_next (std::move (stk));
  m_op_drained = false;
  return true;
}

bool
op_tr_closure::send_to_op ()
{
  if (m_stks.empty ())
    return m_is_plus ? send_to_op (next_from_upstream ()) : false;

  send_to_op (std::make_unique <stack> (*m_stks.back ()));
  m_stks.pop_back ();
  return true;
}

stack::uptr
op_tr_closure::next ()
{
  do
    while (std::shared_ptr <stack> stk = next_from_op ())
      if (auto ret = yield_and_cache (stk))
	return ret;
  while (send_to_op ());

  if (! m_is_plus)
    if (std::shared_ptr <stack> stk = next_from_upstream ())
      return yield_and_cache (stk);

  return nullptr;
}

void
op_tr_closure::reset_me ()
{
  m_stks.clear ();
  m_seen.clear ();
}

void
op_tr_closure::reset ()
{
  reset_me ();
  inner_op::reset ();
}

std::string
op_tr_closure::name () const
{
  return std::string ("close<") + m_upstream->name () + ">";
}


op_subx::op_subx (std::shared_ptr <op> upstream,
		  std::shared_ptr <op_origin> origin,
		  std::shared_ptr <op> op,
		  size_t keep)
  : inner_op {upstream}
  , m_origin {origin}
  , m_op {op}
  , m_keep {keep}
{}

stack::uptr
op_subx::next ()
{
  while (true)
    {
      while (m_stk == nullptr)
	if (m_stk = m_upstream->next ())
	  {
	    m_op->reset ();
	    m_origin->set_next (std::make_unique <stack> (*m_stk));
	  }
	else
	  return nullptr;

      if (auto stk = m_op->next ())
	{
	  auto ret = std::make_unique <stack> (*m_stk);
	  std::vector <std::unique_ptr <value>> kept;
	  for (size_t i = 0; i < m_keep; ++i)
	    kept.push_back (stk->pop ());
	  for (size_t i = 0; i < m_keep; ++i)
	    {
	      ret->push (std::move (kept.back ()));
	      kept.pop_back ();
	    }
	  return ret;
	}

      reset_me ();
    }
}

void
op_subx::reset_me ()
{
  m_stk = nullptr;
}

void
op_subx::reset ()
{
  reset_me ();
  inner_op::reset ();
}

std::string
op_subx::name () const
{
  return std::string ("subx<") + m_op->name () + ">";
}

stack::uptr
op_f_debug::next ()
{
  while (auto stk = m_upstream->next ())
    {
      debug_stack (*stk);
      return stk;
    }
  return nullptr;
}

std::string
op_f_debug::name () const
{
  return "f_debug";
}


struct op_scope::pimpl
{
  std::shared_ptr <op> m_upstream;
  std::shared_ptr <op_origin> m_origin;
  std::shared_ptr <op> m_op;
  size_t m_num_vars;
  bool m_primed;

  pimpl (std::shared_ptr <op> upstream,
	 std::shared_ptr <op_origin> origin,
	 std::shared_ptr <op> op,
	 size_t num_vars)
    : m_upstream {upstream}
    , m_origin {origin}
    , m_op {op}
    , m_num_vars {num_vars}
    , m_primed {false}
  {}

  void
  reset_me ()
  {
    m_primed = false;
  }

  stack::uptr
  next ()
  {
    while (true)
      {
	while (! m_primed)
	  if (auto stk = m_upstream->next ())
	    {
	      // Push new stack frame.
	      stk->set_frame (std::make_shared <frame> (stk->nth_frame (0),
							m_num_vars));
	      m_op->reset ();
	      m_origin->set_next (std::move (stk));
	      m_primed = true;
	    }
	  else
	    return nullptr;

	if (auto stk = m_op->next ())
	  {
	    // Pop top stack frame.
	    std::shared_ptr <frame> of = stk->nth_frame (0);
	    stk->set_frame (stk->nth_frame (1));
	    value_closure::maybe_unlink_frame (of);
	    return stk;
	  }

	reset_me ();
      }
  }

  ~pimpl ()
  {}

  void
  reset ()
  {
    reset_me ();
    m_upstream->reset ();
  }
};

op_scope::op_scope (std::shared_ptr <op> upstream,
		    std::shared_ptr <op_origin> origin,
		    std::shared_ptr <op> op,
		    size_t num_vars)
  : m_pimpl {std::make_unique <pimpl> (upstream, origin, op, num_vars)}
{}

op_scope::~op_scope ()
{}

stack::uptr
op_scope::next ()
{
  return m_pimpl->next ();
}

void
op_scope::reset ()
{
  return m_pimpl->reset ();
}

std::string
op_scope::name () const
{
  return std::string ("scope<vars=") + std::to_string (m_pimpl->m_num_vars)
    + ", " + m_pimpl->m_op->name () + ">";
}


stack::uptr
op_bind::next ()
{
  if (auto stk = m_upstream->next ())
    {
      auto frame = stk->nth_frame (m_depth);
      frame->bind_value (m_index, stk->pop ());
      return stk;
    }
  return nullptr;
}

std::string
op_bind::name () const
{
  return std::string ("bind<") + std::to_string (m_index)
    + "@" + std::to_string (m_depth) + ">";
}


op_read::op_read (std::shared_ptr <op> upstream, size_t depth, var_id index)
  : inner_op {upstream}
  , m_depth {depth}
  , m_index {index}
{}

stack::uptr
op_read::next ()
{
  if (auto stk = m_upstream->next ())
    {
      auto frame = stk->nth_frame (m_depth);
      value &val = frame->read_value (m_index);
      stk->push (val.clone ());
      return stk;
    }
  else
    return nullptr;
}

std::string
op_read::name () const
{
  return std::string ("read<") + std::to_string (m_index)
    + "@" + std::to_string (m_depth) + ">";
}


stack::uptr
op_lex_closure::next ()
{
  if (auto stk = m_upstream->next ())
    {
      stk->push (std::make_unique <value_closure> (m_t, stk->nth_frame (0), 0));
      return stk;
    }
  return nullptr;
}

std::string
op_lex_closure::name () const
{
  return "lex_closure";
}


op_ifelse::op_ifelse (std::shared_ptr <op> upstream,
		      std::shared_ptr <op_origin> cond_origin,
		      std::shared_ptr <op> cond_op,
		      std::shared_ptr <op_origin> then_origin,
		      std::shared_ptr <op> then_op,
		      std::shared_ptr <op_origin> else_origin,
		      std::shared_ptr <op> else_op)
  : inner_op {upstream}
  , m_cond_origin {cond_origin}
  , m_cond_op {cond_op}
  , m_then_origin {then_origin}
  , m_then_op {then_op}
  , m_else_origin {else_origin}
  , m_else_op {else_op}
{
    reset_me ();
}

void
op_ifelse::reset_me ()
{
  m_sel_origin = nullptr;
  m_sel_op = nullptr;
}

void
op_ifelse::reset ()
{
  reset_me ();
  inner_op::reset ();
}

stack::uptr
op_ifelse::next ()
{
  while (true)
    {
      if (m_sel_op == nullptr)
	{
	  if (auto stk = m_upstream->next ())
	    {
	      m_cond_op->reset ();
	      m_cond_origin->set_next (std::make_unique <stack> (*stk));

	      if (m_cond_op->next () != nullptr)
		{
		  m_sel_origin = m_then_origin;
		  m_sel_op = m_then_op;
		}
	      else
		{
		  m_sel_origin = m_else_origin;
		  m_sel_op = m_else_op;
		}

	      m_sel_op->reset ();
	      m_sel_origin->set_next (std::move (stk));
	    }
	  else
	    return nullptr;
	}

      if (auto stk = m_sel_op->next ())
	return stk;

      reset_me ();
    }
}

std::string
op_ifelse::name () const
{
  return "ifelse";
}


pred_result
pred_not::result (stack &stk) const
{
  return ! m_a->result (stk);
}

std::string
pred_not::name () const
{
  return std::string ("not<") + m_a->name () + ">";
}


pred_result
pred_and::result (stack &stk) const
{
  return m_a->result (stk) && m_b->result (stk);
}

std::string
pred_and::name () const
{
  return std::string ("and<") + m_a->name () + "><" + m_b->name () + ">";
}


pred_result
pred_or::result (stack &stk) const
{
  return m_a->result (stk) || m_b->result (stk);
}

std::string
pred_or::name () const
{
  return std::string ("or<") + m_a->name () + "><" + m_b->name () + ">";
}

pred_result
pred_subx_any::result (stack &stk) const
{
  m_op->reset ();
  m_origin->set_next (std::make_unique <stack> (stk));
  if (m_op->next () != nullptr)
    return pred_result::yes;
  else
    return pred_result::no;
}

std::string
pred_subx_any::name () const
{
  return std::string ("pred_subx_any<") + m_op->name () + ">";
}

void
pred_subx_any::reset ()
{
  m_op->reset ();
}


pred_result
pred_subx_compare::result (stack &stk) const
{
  m_op1->reset ();
  m_origin->set_next (std::make_unique <stack> (stk));
  while (auto stk_1 = m_op1->next ())
    {
      m_op2->reset ();
      m_origin->set_next (std::make_unique <stack> (stk));

      while (auto stk_2 = m_op2->next ())
	{
	  stk_1->push (stk_2->pop ());

	  if (m_pred->result (*stk_1) == pred_result::yes)
	    return pred_result::yes;

	  stk_1->pop ();
	}
    }

  return pred_result::no;
}

std::string
pred_subx_compare::name () const
{
  return std::string ("pred_subx_compare<") + m_op1->name () + "><"
    + m_op2->name () + "><" + m_pred->name () + ">";
}

void
pred_subx_compare::reset ()
{
  m_op1->reset ();
  m_op2->reset ();
  m_pred->reset ();
}

pred_result
pred_pos::result (stack &stk) const
{
    auto const &value = stk.top ();
    return value.get_pos () == m_pos ? pred_result::yes : pred_result::no;
}

std::string
pred_pos::name () const
{
  std::stringstream ss;
  ss << "pred_pos<" << m_pos << ">";
  return ss.str ();
}
