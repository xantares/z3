/*++
Copyright (c) 2022 Microsoft Corporation

Module Name:

    mbp_arrays_tg.cpp

Abstract:

    Apply rules for model based projection for arrays on a term graph

Author:

    Hari Govind V K (hgvk94) 2023-03-07

Revision History:

--*/

#include "qe/mbp/mbp_arrays_tg.h"
#include "ast/array_decl_plugin.h"
#include "ast/array_peq.h"
#include "qe/mbp/mbp_qel_util.h"
#include "util/obj_hashtable.h"
#include "util/obj_pair_hashtable.h"

namespace mbp {

struct mbp_array_tg::impl {
    typedef std::pair<expr *, expr *> expr_pair;
    ast_manager &m;
    array_util m_array_util;
    mbp::term_graph &m_tg;
    // TODO: cache mdl evaluation eventhough we extend m_mdl
    model &m_mdl;

    // set of variables on which to apply MBP rules
    obj_hashtable<app> &m_vars_set;

    // variables created in the last iteration of MBP application
    app_ref_vector m_new_vars;

    expr_sparse_mark &m_seen;
    obj_pair_hashtable<expr, expr> m_seenp;

    // apply rules that split on model
    bool m_use_mdl;

    // m_has_store.is_marked(t) if t has a subterm store(v) where v is a
    // variable to be eliminated
    ast_mark m_has_stores;
    // variables required for applying rules
    vector<expr_ref_vector> indices;
    expr_ref_vector terms, rdTerms;

    bool has_var(expr *t) { return contains_vars(t, m_vars_set, m); }

    bool has_arr_var(expr *t) {
        return contains_vars(t, m_vars_set, m, m_array_util.get_family_id(),
                             ARRAY_SORT);
    }

    bool is_var(expr *t) { return is_uninterp_const(t) && has_var(t); }

    bool is_wr_on_rhs(expr *e) {
        return is_app(e) && is_partial_eq(to_app(e)) &&
               is_wr_on_rhs(to_app(e)->get_arg(0), to_app(e)->get_arg(1));
    }

    bool is_wr_on_rhs(expr *lhs, expr *rhs) {
        return (is_arr_write(rhs) && !is_arr_write(lhs));
    }

    bool is_arr_write(expr *t) {
        if (!m_array_util.is_store(t)) return false;
        return has_var(to_app(t));
    }

    // Returns true if e has a subterm store(v) where v is a variable to be
    // eliminated. Assumes that has_store has already been called for
    // subexpressions of e
    bool has_stores(expr *e) {
        if (m_has_stores.is_marked(e)) return true;
        if (!is_app(e)) return false;
        if (m_array_util.is_store(e) && is_var(to_app(e)->get_arg(0))) {
            m_has_stores.mark(e, true);
            return true;
        }
        for (auto c : *(to_app(e))) {
            if (m_has_stores.is_marked(c)) {
                m_has_stores.mark(e, true);
                return true;
            }
        }
        return false;
    }

    bool is_rd_wr(expr *t) {
        if (!m_array_util.is_select(t)) return false;
        return m_array_util.is_store(to_app(t)->get_arg(0)) &&
               has_stores(to_app(t)->get_arg(0));
    }

    bool is_implicit_peq(expr *e) {
        return m.is_eq(e) &&
               is_implicit_peq(to_app(e)->get_arg(0), to_app(e)->get_arg(1));
    }

    bool is_implicit_peq(expr *lhs, expr *rhs) {
        return m_array_util.is_array(lhs) && m_array_util.is_array(rhs) &&
               (has_var(lhs) || has_var(rhs));
    }

    void mark_seen(expr *t) { m_seen.mark(t); }
    bool is_seen(expr *t) { return m_seen.is_marked(t); }
    void mark_seen(expr *t1, expr *t2) { m_seenp.insert(expr_pair(t1, t2)); }
    bool is_seen(expr *t1, expr *t2) {
        return m_seenp.contains(expr_pair(t1, t2)) ||
               m_seenp.contains(expr_pair(t2, t1));
    }

    impl(ast_manager &man, mbp::term_graph &tg, model &mdl,
         obj_hashtable<app> &vars_set, expr_sparse_mark &seen)
        : m(man), m_array_util(m), m_tg(tg), m_mdl(mdl), m_vars_set(vars_set),
          m_new_vars(m), m_seen(seen), m_use_mdl(false), terms(m), rdTerms(m) {}

    // create a peq where write terms are preferred  on the left hand side
    peq mk_wr_peq(expr *e1, expr *e2) {
        vector<expr_ref_vector> empty;
        return mk_wr_peq(e1, e2, empty);
    }

    // create a peq where write terms are preferred on the left hand side
    peq mk_wr_peq(expr *e1, expr *e2, vector<expr_ref_vector> &indices) {
        expr *n_lhs = e1, *n_rhs = e2;
        if (is_wr_on_rhs(e1, e2)) std::swap(n_lhs, n_rhs);
        return peq(n_lhs, n_rhs, indices, m);
    }

    // rewrite          store(x, j, elem) \peq_{indices} y
    // into either      j = i && x \peq_{indices} y        (for some i in
    // indices) or               &&_{i \in indices} j \neq i &&
    //                        x \peq_{indices, j} y &&
    //                        select(y, j) = elem
    // rewrite negation !(store(x, j, elem) \peq_{indices} y) into
    // into either      j = i && !(x \peq_{indices} y)        (for some i in
    // indices) or               &&_{i \in indices} j \neq i &&
    //                        !(x \peq_{indices, j} y) &&
    // or              &&_{i \in indices} j \neq i &&
    //                        !(select(y, j) = elem)
    void elimwreq(peq p, bool is_neg) {
        SASSERT(is_arr_write(p.lhs()));
        TRACE("mbp_tg",
              tout << "applying elimwreq on " << expr_ref(p.mk_peq(), m););
        vector<expr_ref_vector> indices;
        expr *j = to_app(p.lhs())->get_arg(1);
        expr *elem = to_app(p.lhs())->get_arg(2);
        bool in = false;
        p.get_diff_indices(indices);
        expr_ref eq_index(m);
        expr_ref_vector deq(m);
        for (expr_ref_vector &e : indices) {
            for (expr *i : e) {
                if (m_mdl.are_equal(j, i)) {
                    in = true;
                    // save for later
                    eq_index = i;
                    break;
                } else
                    deq.push_back(i);
            }
        }
        if (in) {
            SASSERT(m_mdl.are_equal(j, eq_index));
            peq p_new =
                mk_wr_peq(to_app(p.lhs())->get_arg(0), p.rhs(), indices);
            m_tg.add_eq(j, eq_index);
            expr_ref p_new_expr(m);
            p_new_expr = is_neg ? m.mk_not(p_new.mk_peq()) : p_new.mk_peq();
            m_tg.add_lit(p_new_expr);
            m_tg.add_eq(p_new_expr, p.mk_peq());
            return;
        }
        for (expr *d : deq) { m_tg.add_deq(j, d); }
        expr_ref_vector setOne(m);
        setOne.push_back(j);
        indices.push_back(setOne);
        peq p_new = mk_wr_peq(to_app(p.lhs())->get_arg(0), p.rhs(), indices);
        expr *args[2] = {p.rhs(), j};
        expr_ref rd(m_array_util.mk_select(2, args), m);
        if (!is_neg) {
            m_tg.add_lit(p_new.mk_peq());
            m_tg.add_eq(rd, elem);
            m_tg.add_eq(p.mk_peq(), p_new.mk_peq());
        } else {
            SASSERT(m_mdl.is_false(p_new.mk_peq()) ||
                    !m_mdl.are_equal(rd, elem));
            if (m_mdl.is_false(p_new.mk_peq())) {
                expr_ref npeq(mk_not(p_new.mk_peq()), m);
                m_tg.add_lit(npeq);
                m_tg.add_eq(p.mk_peq(), p_new.mk_peq());
            }
            if (!m_mdl.are_equal(rd, elem)) { m_tg.add_deq(rd, elem); }
        }
    }

    // add equality v = rd where v is a fresh variable
    void add_rdVar(expr *rd) {
        // do not assign new variable if rd is already equal to a value
        if (m_tg.has_val_in_class(rd)) return;
        TRACE("mbp_tg", tout << "applying add_rdVar on " << expr_ref(rd, m););
        app_ref u = new_var(to_app(rd)->get_sort(), m);
        m_new_vars.push_back(u);
        m_tg.add_var(u);
        m_tg.add_eq(u, rd);
        m_mdl.register_decl(u->get_decl(), m_mdl(rd));
    }

    // given a \peq_{indices} t, where a is a variable, merge equivalence class
    // of a with store(t, indices, elems) where elems are fresh constants
    void elimeq(peq p) {
        TRACE("mbp_tg",
              tout << "applying elimeq on " << expr_ref(p.mk_peq(), m););
        app_ref_vector aux_consts(m);
        expr_ref eq(m);
        expr_ref sel(m);
        eq = p.mk_eq(aux_consts, true);
        vector<expr_ref_vector> indices;
        p.get_diff_indices(indices);
        vector<expr_ref_vector>::iterator itr = indices.begin();
        unsigned i = 0;
        for (app *a : aux_consts) {
            m_new_vars.push_back(a);
            m_tg.add_var(a);
            auto const &indx = std::next(itr, i);
            SASSERT(indx->size() == 1);
            expr *args[2] = {to_app(p.lhs()), to_app(indx->get(0))};
            sel = m_array_util.mk_select(2, args);
            m_mdl.register_decl(a->get_decl(), m_mdl(sel));
            i++;
        }
        m_tg.add_lit(eq);
        m_tg.add_eq(p.mk_peq(), m.mk_true());
        TRACE("mbp_tg", tout << "added lit  " << eq;);
    }

    // rewrite select(store(a, i, k), j) into either select(a, j) or k
    void elimrdwr(expr *term) {
        SASSERT(is_rd_wr(term));
        TRACE("mbp_tg", tout << "applying elimrdwr on " << expr_ref(term, m););
        expr *wr_ind = to_app(to_app(term)->get_arg(0))->get_arg(1);
        expr *rd_ind = to_app(term)->get_arg(1);
        expr *e;
        if (m_mdl.are_equal(wr_ind, rd_ind)) {
            m_tg.add_eq(wr_ind, rd_ind);
            e = to_app(to_app(term)->get_arg(0))->get_arg(2);
        } else {
            m_tg.add_deq(wr_ind, rd_ind);
            expr *args[2] = {to_app(to_app(term)->get_arg(0))->get_arg(0),
                             to_app(term)->get_arg(1)};
            e = m_array_util.mk_select(2, args);
        }
        m_tg.add_eq(term, e);
    }

    // iterate through all terms in m_tg and apply all array MBP rules once
    // returns true if any rules were applied
    bool apply() {
        TRACE("mbp_tg", tout << "Iterating over terms of tg";);
        indices.reset();
        rdTerms.reset();
        m_new_vars.reset();
        expr_ref e(m), rdEq(m), rdDeq(m);
        expr *nt, *term;
        bool progress = false, is_neg = false;

        // Not resetting terms because get_terms calls resize on terms
        m_tg.get_terms(terms, false);
        for (unsigned i = 0; i < terms.size(); i++) {
            term = terms.get(i);
            if (m_seen.is_marked(term)) continue;
            if (m_tg.is_cgr(term)) continue;
            TRACE("mbp_tg", tout << "processing " << expr_ref(term, m););
            if (is_implicit_peq(term)) {
                // rewrite array eq as peq
                mark_seen(term);
                progress = true;
                e = mk_wr_peq(to_app(term)->get_arg(0),
                              to_app(term)->get_arg(1))
                        .mk_peq();
                m_tg.add_lit(e);
                m_tg.add_eq(term, e);
                continue;
            }
            nt = term;
            is_neg = m.is_not(term, nt);
            if (is_app(nt) && is_partial_eq(to_app(nt))) {
                peq p(to_app(nt), m);
                if (m_use_mdl && is_arr_write(p.lhs())) {
                    mark_seen(nt);
                    mark_seen(term);
                    progress = true;
                    elimwreq(p, is_neg);
                    continue;
                }
                if (!m_array_util.is_store(p.lhs()) && has_var(p.lhs())) {
                    // TODO: don't apply this rule if vars in p.lhs() also
                    // appear in p.rhs()
                    mark_seen(p.lhs());
                    mark_seen(nt);
                    mark_seen(term);
                    progress = true;
                    elimeq(p);
                    continue;
                }
                // eliminate eq when the variable is on the rhs
                if (!m_array_util.is_store(p.rhs()) && has_var(p.rhs())) {
                    mark_seen(p.rhs());
                    p.get_diff_indices(indices);
                    peq p_new = mk_wr_peq(p.rhs(), p.lhs(), indices);
                    mark_seen(nt);
                    mark_seen(term);
                    progress = true;
                    elimeq(p_new);
                    continue;
                }
            }
            if (m_use_mdl && is_rd_wr(term)) {
                mark_seen(term);
                progress = true;
                elimrdwr(term);
                continue;
            }
        }

        // iterate over term graph again to collect read terms
        // irrespective of whether they have been marked or not
        rdTerms.reset();
        for (unsigned i = 0; i < terms.size(); i++) {
            term = terms.get(i);
            if (m_array_util.is_select(term) &&
                has_var(to_app(term)->get_arg(0))) {
                rdTerms.push_back(term);
                if (is_seen(term)) continue;
                add_rdVar(term);
                mark_seen(term);
            }
        }
        if (!m_use_mdl) return progress;
        expr *e1, *e2, *a1, *a2, *i1, *i2;
        for (unsigned i = 0; i < rdTerms.size(); i++) {
            e1 = rdTerms.get(i);
            a1 = to_app(e1)->get_arg(0);
            i1 = to_app(e1)->get_arg(1);
            for (unsigned j = i + 1; j < rdTerms.size(); j++) {
                e2 = rdTerms.get(j);
                a2 = to_app(e2)->get_arg(0);
                i2 = to_app(e2)->get_arg(1);
                if (!is_seen(e1, e2) && a1->get_id() == a2->get_id()) {
                    mark_seen(e1, e2);
                    progress = true;
                    if (m_mdl.are_equal(i1, i2)) {
                        m_tg.add_eq(i1, i2);
                    } else {
                        SASSERT(!m_mdl.are_equal(i1, i2));
                        m_tg.add_deq(i1, i2);
                    }
                    continue;
                }
            }
        }
        return progress;
    }
};

void mbp_array_tg::use_model() { m_impl->m_use_mdl = true; }
bool mbp_array_tg::apply() { return m_impl->apply(); }
void mbp_array_tg::reset() {
    m_impl->m_seen.reset();
    m_impl->m_vars_set.reset();
}
void mbp_array_tg::get_new_vars(app_ref_vector *&t) { t = &m_impl->m_new_vars; }
family_id mbp_array_tg::get_family_id() const {
    return m_impl->m_array_util.get_family_id();
}
mbp_array_tg::mbp_array_tg(ast_manager &man, mbp::term_graph &tg, model &mdl,
                           obj_hashtable<app> &vars_set,
                           expr_sparse_mark &seen) {
    m_impl = alloc(mbp_array_tg::impl, man, tg, mdl, vars_set, seen);
}
mbp_array_tg::~mbp_array_tg() { dealloc(m_impl); }

} // namespace mbp
