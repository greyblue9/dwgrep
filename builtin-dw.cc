#include <memory>
#include "make_unique.hh"

#include "builtin.hh"
#include "dwit.hh"
#include "op.hh"
#include "dwpp.hh"
#include "dwcst.hh"

static struct
  : public builtin
{
  struct winfo
    : public op
  {
    std::shared_ptr <op> m_upstream;
    dwgrep_graph::sptr m_gr;
    all_dies_iterator m_it;
    valfile::uptr m_vf;
    size_t m_pos;

    winfo (std::shared_ptr <op> upstream, dwgrep_graph::sptr gr)
      : m_upstream {upstream}
      , m_gr {gr}
      , m_it {all_dies_iterator::end ()}
      , m_pos {0}
    {}

    void
    reset_me ()
    {
      m_vf = nullptr;
      m_pos = 0;
    }

    valfile::uptr
    next () override
    {
      while (true)
	{
	  if (m_vf == nullptr)
	    {
	      m_vf = m_upstream->next ();
	      if (m_vf == nullptr)
		return nullptr;
	      m_it = all_dies_iterator (&*m_gr->dwarf);
	    }

	  if (m_it != all_dies_iterator::end ())
	    {
	      auto ret = std::make_unique <valfile> (*m_vf);
	      auto v = std::make_unique <value_die> (m_gr, **m_it, m_pos++);
	      ret->push (std::move (v));
	      ++m_it;
	      return ret;
	    }

	  reset_me ();
	}
    }

    void
    reset () override
    {
      reset_me ();
      m_upstream->reset ();
    }

    std::string
    name () const override
    {
      return "winfo";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <winfo> (upstream, q);
  }

  char const *
  name () const override
  {
    return "winfo";
  }
} builtin_winfo;

static struct
  : public builtin
{
  struct unit
    : public op
  {
    std::shared_ptr <op> m_upstream;
    dwgrep_graph::sptr m_gr;
    valfile::uptr m_vf;
    all_dies_iterator m_it;
    all_dies_iterator m_end;
    size_t m_pos;

    unit (std::shared_ptr <op> upstream, dwgrep_graph::sptr gr)
      : m_upstream {upstream}
      , m_gr {gr}
      , m_it {all_dies_iterator::end ()}
      , m_end {all_dies_iterator::end ()}
      , m_pos {0}
    {}

    void
    init_from_die (Dwarf_Die die)
    {
      Dwarf_Die cudie;
      if (dwarf_diecu (&die, &cudie, nullptr, nullptr) == nullptr)
	throw_libdw ();

      cu_iterator cuit {&*m_gr->dwarf, cudie};
      m_it = all_dies_iterator (cuit);
      m_end = all_dies_iterator (++cuit);
    }

    void
    reset_me ()
    {
      m_vf = nullptr;
      m_it = all_dies_iterator::end ();
      m_pos = 0;
    }

    valfile::uptr
    next () override
    {
      while (true)
	{
	  while (m_vf == nullptr)
	    {
	      if (auto vf = m_upstream->next ())
		{
		  auto vp = vf->pop ();
		  if (auto v = value::as <value_die> (&*vp))
		    {
		      init_from_die (v->get_die ());
		      m_vf = std::move (vf);
		    }
		  else if (auto v = value::as <value_attr> (&*vp))
		    {
		      init_from_die (v->get_die ());
		      m_vf = std::move (vf);
		    }
		  else
		    std::cerr << "Error: `unit' expects a T_NODE or "
		      "T_ATTR on TOS.\n";
		}
	      else
		return nullptr;
	    }

	  if (m_it != m_end)
	    {
	      auto ret = std::make_unique <valfile> (*m_vf);
	      ret->push (std::make_unique <value_die> (m_gr, **m_it, m_pos++));
	      ++m_it;
	      return ret;
	    }

	  reset_me ();
	}
    }

    void
    reset () override
    {
      reset_me ();
      m_upstream->reset ();
    }

    std::string
    name () const override
    {
      return "unit";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <unit> (upstream, q);
  }

  char const *
  name () const override
  {
    return "unit";
  }
} builtin_unit;

static struct
  : public builtin
{
  struct child
    : public op
  {
    std::shared_ptr <op> m_upstream;
    dwgrep_graph::sptr m_gr;
    valfile::uptr m_vf;
    Dwarf_Die m_child;

    size_t m_pos;

    child (std::shared_ptr <op> upstream, dwgrep_graph::sptr gr)
      : m_upstream {upstream}
      , m_gr {gr}
      , m_child {}
      , m_pos {0}
    {}

    void
    reset_me ()
    {
      m_vf = nullptr;
      m_pos = 0;
    }

    valfile::uptr
    next () override
    {
      while (true)
	{
	  while (m_vf == nullptr)
	    {
	      if (auto vf = m_upstream->next ())
		{
		  auto vp = vf->pop ();
		  if (auto v = value::as <value_die> (&*vp))
		    {
		      Dwarf_Die *die = &v->get_die ();
		      if (dwarf_haschildren (die))
			{
			  if (dwarf_child (die, &m_child) != 0)
			    throw_libdw ();

			  // We found our guy.
			  m_vf = std::move (vf);
			}
		    }
		  else
		    std::cerr << "Error: `child' expects a T_NODE on TOS.\n";
		}
	      else
		return nullptr;
	    }

	  auto ret = std::make_unique <valfile> (*m_vf);
	  ret->push (std::make_unique <value_die> (m_gr, m_child, m_pos++));

	  switch (dwarf_siblingof (&m_child, &m_child))
	    {
	    case -1:
	      throw_libdw ();
	    case 1:
	      // No more siblings.
	      reset_me ();
	      break;
	    case 0:
	      break;
	    }

	  return ret;
	}
    }

    void
    reset () override
    {
      reset_me ();
      m_upstream->reset ();
    }

    std::string
    name () const override
    {
      return "child";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <child> (upstream, q);
  }

  char const *
  name () const override
  {
    return "child";
  }
} builtin_child;

static struct
  : public builtin
{
  struct attribute
    : public op
  {
    std::shared_ptr <op> m_upstream;
    dwgrep_graph::sptr m_gr;
    Dwarf_Die m_die;
    valfile::uptr m_vf;
    attr_iterator m_it;

    size_t m_pos;

    attribute (std::shared_ptr <op> upstream, dwgrep_graph::sptr gr)
      : m_upstream {upstream}
      , m_gr {gr}
      , m_die {}
      , m_it {attr_iterator::end ()}
      , m_pos {0}
    {}

    void
    reset_me ()
    {
      m_vf = nullptr;
      m_pos = 0;
    }

    valfile::uptr
    next ()
    {
      while (true)
	{
	  while (m_vf == nullptr)
	    {
	      if (auto vf = m_upstream->next ())
		{
		  auto vp = vf->pop ();
		  if (auto v = value::as <value_die> (&*vp))
		    {
		      m_die = v->get_die ();
		      m_it = attr_iterator (&m_die);
		      m_vf = std::move (vf);
		    }
		  else
		    std::cerr
		      << "Error: `attribute' expects a T_NODE on TOS.\n";
		}
	      else
		return nullptr;
	    }

	  if (m_it != attr_iterator::end ())
	    {
	      auto ret = std::make_unique <valfile> (*m_vf);
	      ret->push (std::make_unique <value_attr>
			 (m_gr, **m_it, m_die, m_pos++));
	      ++m_it;
	      return ret;
	    }

	  reset_me ();
	}
    }

    void
    reset ()
    {
      reset_me ();
      m_upstream->reset ();
    }

    std::string
    name () const override
    {
      return "attribute";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <attribute> (upstream, q);
  }

  char const *
  name () const override
  {
    return "attribute";
  }
} builtin_attribute;

static struct
  : public builtin
{
  struct offset
    : public dwop_f
  {
    using dwop_f::dwop_f;

    bool
    operate (valfile &vf, Dwarf_Die &die) override
    {
      Dwarf_Off off = dwarf_dieoffset (&die);
      auto cst = constant {off, &hex_constant_dom};
      vf.push (std::make_unique <value_cst> (cst, 0));
      return true;
    }

    std::string
    name () const override
    {
      return "offset";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <offset> (upstream, q);
  }

  char const *
  name () const override
  {
    return "offset";
  }
} builtin_offset;

namespace
{
  bool
  operate_tag (valfile &vf, Dwarf_Die &die)
  {
    int tag = dwarf_tag (&die);
    assert (tag >= 0);
    constant cst {(unsigned) tag, &dw_tag_dom};
    vf.push (std::make_unique <value_cst> (cst, 0));
    return true;
  }
}

static struct
  : public builtin
{
  struct op
    : public dwop_f
  {
    using dwop_f::dwop_f;

    bool
    operate (valfile &vf, Dwarf_Die &die) override
    {
      return operate_tag (vf, die);
    }

    bool
    operate (valfile &vf, Dwarf_Attribute &attr, Dwarf_Die &die) override
    {
      unsigned name = dwarf_whatattr (&attr);
      constant cst {name, &dw_attr_dom};
      vf.push (std::make_unique <value_cst> (cst, 0));
      return true;
    }

    std::string
    name () const override
    {
      return "name";
    }
  };

  std::shared_ptr < ::op>
  build_exec (std::shared_ptr < ::op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <op> (upstream, q);
  }

  char const *
  name () const override
  {
    return "name";
  }
} builtin_name;

static struct
  : public builtin
{
  struct tag
    : public dwop_f
  {
    using dwop_f::dwop_f;

    bool
    operate (valfile &vf, Dwarf_Die &die) override
    {
      return operate_tag (vf, die);
    }

    std::string
    name () const override
    {
      return "tag";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <tag> (upstream, q);
  }

  char const *
  name () const override
  {
    return "tag";
  }
} builtin_tag;

static struct
  : public builtin
{
  struct form
    : public dwop_f
  {
    using dwop_f::dwop_f;

    bool
    operate (valfile &vf, Dwarf_Attribute &attr, Dwarf_Die &die) override
    {
      unsigned name = dwarf_whatform (&attr);
      constant cst {name, &dw_form_dom};
      vf.push (std::make_unique <value_cst> (cst, 0));
      return true;
    }

    std::string
    name () const override
    {
      return "form";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <form> (upstream, q);
  }

  char const *
  name () const override
  {
    return "form";
  }
} builtin_form;

static struct
  : public builtin
{
  struct parent
    : public dwop_f
  {
    using dwop_f::dwop_f;

    bool
    operate (valfile &vf, Dwarf_Die &die) override
    {
      Dwarf_Off par_off = m_g->find_parent (die);
      if (par_off == dwgrep_graph::none_off)
	return false;

      Dwarf_Die par_die;
      if (dwarf_offdie (&*m_g->dwarf, par_off, &par_die) == nullptr)
	throw_libdw ();

      vf.push (std::make_unique <value_die> (m_g, par_die, 0));
      return true;
    }

    bool
    operate (valfile &vf, Dwarf_Attribute &attr, Dwarf_Die &die) override
    {
      vf.push (std::make_unique <value_die> (m_g, die, 0));
      return true;
    }

    std::string
    name () const override
    {
      return "parent";
    }
  };

  std::shared_ptr <op>
  build_exec (std::shared_ptr <op> upstream, dwgrep_graph::sptr q,
	      std::shared_ptr <scope> scope) const override
  {
    return std::make_shared <parent> (upstream, q);
  }

  char const *
  name () const override
  {
    return "parent";
  }
} builtin_parent;

static struct register_dw
{
  register_dw ()
  {
    add_builtin (builtin_winfo);
    add_builtin (builtin_unit);

    add_builtin (builtin_child);
    add_builtin (builtin_attribute);
    add_builtin (builtin_offset);
    add_builtin (builtin_name);
    add_builtin (builtin_tag);
    add_builtin (builtin_form);
    add_builtin (builtin_parent);
  }
} register_dw;
