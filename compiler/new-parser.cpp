// vim: set ts=8 sts=4 sw=4 tw=99 et:
//  Pawn compiler - Recursive descend expresion parser
//
//  Copyright (c) ITB CompuPhase, 1997-2005
//
//  This software is provided "as-is", without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1.  The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software in
//      a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//  2.  Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//  3.  This notice may not be removed or altered from any source distribution.
//
//  Version: $Id$

#include <assert.h>
#include <string.h>

#include <amtl/am-raii.h>
#include "emitter.h"
#include "errors.h"
#include "lexer.h"
#include "new-parser.h"
#include "optimizer.h"
#include "parse-node.h"
#include "sc.h"
#include "sclist.h"
#include "sctracker.h"
#include "scvars.h"
#include "semantics.h"
#include "types.h"

using namespace sp;

bool Parser::sInPreprocessor = false;
bool Parser::sDetectedIllegalPreprocessorSymbols = false;

void
Parser::parse()
{
    ke::SaveAndSet<bool> limit_errors(&sc_one_error_per_statement, true);

    while (freading) {
        Stmt* decl = nullptr;

        token_t tok;
        switch (lextok(&tok)) {
            case 0:
                /* ignore zero's */
                break;
            case tSYMBOL:
                // Fallthrough.
            case tINT:
            case tOBJECT:
            case tCHAR:
            case tVOID:
            case tLABEL:
                lexpush();
                // Fallthrough.
            case tNEW:
            case tSTATIC:
            case tPUBLIC:
            case tSTOCK:
            case tOPERATOR:
            case tNATIVE:
            case tFORWARD:
                decl = parse_unknown_decl(&tok);
                break;
            case tSTATIC_ASSERT:
                decl = parse_static_assert();
                break;
            case tFUNCENUM:
            case tFUNCTAG:
                error(FATAL_ERROR_FUNCENUM);
                break;
            case tTYPEDEF:
                decl = parse_typedef();
                break;
            case tTYPESET:
                decl = parse_typeset();
                break;
            case tSTRUCT:
                decl = parse_pstruct();
                break;
            case tCONST:
                decl = parse_const(sGLOBAL);
                break;
            case tENUM:
                if (matchtoken(tSTRUCT))
                    decl_enumstruct();
                else
                    decl = parse_enum(sGLOBAL);
                break;
            case tMETHODMAP:
                domethodmap(Layout_MethodMap);
                break;
            case tUSING:
                decl = parse_using();
                break;
            case '}':
                error(54); /* unmatched closing brace */
                break;
            case '{':
                error(55); /* start of function body without function header */
                break;
            default:
                if (freading) {
                    error(10);    /* illegal function or declaration */
                    lexclr(TRUE); /* drop the rest of the line */
                }
        }

        // Until we can eliminate the two-pass parser, top-level decls must be
        // resolved immediately.
        if (decl)
            decl->Process();
    }
}

Stmt*
Parser::parse_unknown_decl(const token_t* tok)
{
    declinfo_t decl;

    if (tok->id == tNATIVE || tok->id == tFORWARD) {
        parse_decl(&decl, DECLFLAG_MAYBE_FUNCTION);
        funcstub(tok->id, &decl, NULL);
        return nullptr;
    }

    auto pos = current_pos();

    int fpublic = FALSE, fstock = FALSE, fstatic = FALSE;
    switch (tok->id) {
        case tPUBLIC:
            fpublic = TRUE;
            break;
        case tSTOCK:
            fstock = TRUE;
            if (matchtoken(tSTATIC))
                fstatic = TRUE;
            break;
        case tSTATIC:
            fstatic = TRUE;

            // For compatibility, we must include this case. Though "stock" should
            // come first.
            if (matchtoken(tSTOCK))
                fstock = TRUE;
            break;
    }

    int flags = DECLFLAG_MAYBE_FUNCTION | DECLFLAG_VARIABLE | DECLFLAG_ENUMROOT;
    if (tok->id == tNEW)
        flags |= DECLFLAG_OLD;

    if (!parse_decl(&decl, flags)) {
        // Error will have been reported earlier. Reset |decl| so we don't crash
        // thinking tag -1 has every flag.
        decl.type.tag = 0;
    }

    // Hacky bag o' hints as to whether this is a variable decl.
    bool probablyVariable = tok->id == tNEW || decl.type.has_postdims || !lexpeek('(') ||
                            decl.type.is_const;

    if (!decl.opertok && probablyVariable) {
        if (tok->id == tNEW && decl.type.is_new)
            error(143);
        Type* type = gTypes.find(decl.type.tag);
        if (type && type->kind() == TypeKind::Struct) {
            Expr* init = nullptr;
            if (matchtoken('=')) {
                needtoken('{');
                init = struct_init();
            }
            matchtoken(';');
            // Without an initializer, the stock keyword is implied.
            return new VarDecl(pos, gAtoms.add(decl.name), decl.type, sGLOBAL, fpublic && init,
                               false, !init, init);
        }
        VarParams params;
        params.vclass = sGLOBAL;
        params.is_public = !!fpublic;
        params.is_static = !!fstatic;
        params.is_stock = !!fstock;
        return parse_var(&decl, params);
    } else {
        if (!newfunc(&decl, NULL, fpublic, fstatic, fstock, NULL)) {
            // Illegal function or declaration. Drop the line, reset literal queue.
            error(10);
            lexclr(TRUE);
        }
    }
    return nullptr;
}

Stmt*
Parser::parse_var(declinfo_t* decl, const VarParams& params)
{
    StmtList* list = nullptr;
    Stmt* stmt = nullptr;

    for (;;) {
        auto pos = current_pos();
        auto name = gAtoms.add(decl->name);

        Expr* init = nullptr;
        if (matchtoken('='))
            init = var_init(params.vclass);

        VarDecl* var = new VarDecl(pos, name, decl->type, params.vclass, params.is_public,
                                   params.is_static, params.is_stock, init);
        if (!params.autozero)
            var->set_no_autozero();

        if (stmt) {
            if (!list) {
                list = new StmtList(var->pos());
                list->stmts().emplace_back(stmt);
            }
            list->stmts().emplace_back(var);
        } else {
            stmt = var;
        }

        if (!matchtoken(','))
            break;

        if (decl->type.is_new)
            reparse_new_decl(decl, DECLFLAG_VARIABLE | DECLFLAG_ENUMROOT);
        else
            reparse_old_decl(decl, DECLFLAG_VARIABLE | DECLFLAG_ENUMROOT);
    }

    needtoken(tTERM); /* if not comma, must be semicolumn */
    return list ? list : stmt;
}

Decl*
Parser::parse_enum(int vclass)
{
    auto pos = current_pos();

    cell val;
    char* str;
    Atom* label = nullptr;
    if (lex(&val, &str) == tLABEL)
        label = gAtoms.add(str);
    else
        lexpush();

    Atom* name = nullptr;
    if (lex(&val, &str) == tSYMBOL)
        name = gAtoms.add(str);
    else
        lexpush();

    cell increment = 1;
    cell multiplier = 1;
    if (matchtoken('(')) {
        error(228);
        if (matchtoken(taADD)) {
            exprconst(&increment, NULL, NULL);
        } else if (matchtoken(taMULT)) {
            exprconst(&multiplier, NULL, NULL);
        } else if (matchtoken(taSHL)) {
            exprconst(&val, NULL, NULL);
            while (val-- > 0)
                multiplier *= 2;
        }
        needtoken(')');
    }

    EnumDecl* decl = new EnumDecl(pos, vclass, label, name, increment, multiplier);

    needtoken('{');

    cell size;
    do {
        if (matchtoken('}')) {
            lexpush();
            break;
        }
        if (matchtoken(tLABEL))
            error(153);

        sp::Atom* field_name = nullptr;
        if (needtoken(tSYMBOL)) {
            tokeninfo(&val, &str);
            field_name = gAtoms.add(str);
        }

        auto pos = current_pos();

        if (matchtoken('[')) {
            error(153);
            exprconst(&size, nullptr, nullptr);
            needtoken(']');
        }

        Expr* value = nullptr;
        if (matchtoken('='))
            value = hier14();

        if (field_name)
            decl->fields().push_back(EnumField(pos, field_name, value));
    } while (matchtoken(','));

    needtoken('}');
    matchtoken(';');
    return decl;
}

Decl*
Parser::parse_pstruct()
{
    PstructDecl* struct_decl = nullptr;

    auto pos = current_pos();

    token_ident_t ident = {};
    if (needsymbol(&ident))
        struct_decl = new PstructDecl(pos, gAtoms.add(ident.name));

    needtoken('{');
    do {
        if (matchtoken('}')) {
            /* Quick exit */
            lexpush();
            break;
        }

        declinfo_t decl = {};
        decl.type.ident = iVARIABLE;

        needtoken(tPUBLIC);
        auto pos = current_pos();
        if (!parse_new_decl(&decl, nullptr, DECLFLAG_FIELD)) {
            lexclr(TRUE);
            continue;
        }

        if (struct_decl) {
            auto name = gAtoms.add(decl.name);
            struct_decl->fields().push_back(StructField(pos, name, decl.type));
        }

        require_newline(TerminatorPolicy::NewlineOrSemicolon);
    } while (!lexpeek('}'));

    needtoken('}');
    matchtoken(';'); // eat up optional semicolon
    return struct_decl;
}

Decl*
Parser::parse_typedef()
{
    auto pos = current_pos();

    token_ident_t ident;
    if (!needsymbol(&ident))
        return new ErrorDecl();

    needtoken('=');

    auto type = parse_function_type();
    return new TypedefDecl(pos, gAtoms.add(ident.name), type);
}

Decl*
Parser::parse_typeset()
{
    auto pos = current_pos();

    token_ident_t ident;
    if (!needsymbol(&ident))
        return new ErrorDecl();

    TypesetDecl* decl = new TypesetDecl(pos, gAtoms.add(ident.name));

    needtoken('{');
    while (!matchtoken('}')) {
        auto type = parse_function_type();
        decl->types().push_back(type);
    }

    require_newline(TerminatorPolicy::NewlineOrSemicolon);
    return decl;
}

Decl*
Parser::parse_using()
{
    auto pos = current_pos();

    auto validate = []() -> bool {
        token_ident_t ident;
        if (!needsymbol(&ident))
            return false;
        if (strcmp(ident.name, "__intrinsics__") != 0) {
            error(156);
            return false;
        }
        if (!needtoken('.'))
            return false;
        if (!needsymbol(&ident))
            return false;
        if (strcmp(ident.name, "Handle") != 0) {
            error(156);
            return false;
        }
        return true;
    };
    if (!validate()) {
        lexclr(TRUE);
        return new ErrorDecl();
    }

    require_newline(TerminatorPolicy::Semicolon);
    return new UsingDecl(pos);
}

Stmt*
Parser::parse_const(int vclass)
{
    StmtList* list = nullptr;
    Stmt* decl = nullptr;

    do {
        auto pos = current_pos();

        // Since spcomp is terrible, it's hard to use parse_decl() here - there
        // are all sorts of restrictions on const. We just implement some quick
        // detection instead.
        int tag = 0;
        token_t tok;
        switch (lextok(&tok)) {
            case tINT:
            case tOBJECT:
            case tCHAR:
                tag = parse_new_typename(&tok);
                break;
            case tLABEL:
                tag = pc_addtag(tok.str);
                break;
            case tSYMBOL:
                // See if we can peek ahead another symbol.
                if (lexpeek(tSYMBOL)) {
                    // This is a new-style declaration.
                    tag = parse_new_typename(&tok);
                } else {
                    // Otherwise, we got "const X ..." so the tag is int. Give the
                    // symbol back to the lexer so we get it as the name.
                    lexpush();
                }
                break;
            default:
                error(122);
                break;
        }

        sp::Atom* name = nullptr;
        if (expecttoken(tSYMBOL, &tok))
            name = gAtoms.add(tok.str);

        needtoken('=');

        int expr_val, expr_tag;
        exprconst(&expr_val, &expr_tag, nullptr);

        typeinfo_t type = {};
        type.tag = tag;
        type.is_const = true;

        if (!name)
            continue;

        VarDecl* var = new ConstDecl(pos, name, type, vclass, expr_tag, expr_val);
        if (decl) {
            if (!list) {
                list = new StmtList(var->pos());
                list->stmts().push_back(decl);
            }
            list->stmts().push_back(var);
        } else {
            decl = var;
        }
    } while (matchtoken(','));

    needtoken(tTERM);
    return list ? list : decl;
}

int
Parser::expression(value* lval)
{
    Expr* expr = hier14();

    SemaContext sc;
    if (!expr->Bind(sc) || !expr->Analyze(sc)) {
        sideeffect = TRUE;
        *lval = value::ErrorValue();
        return FALSE;
    }
    expr->ProcessUses();

    *lval = expr->val();
    if (cc_ok())
        expr->Emit();

    sideeffect = expr->HasSideEffects();
    return expr->lvalue();
}

Expr*
Parser::hier14()
{
    Expr* node = hier13();

    cell val;
    char* st;
    int tok = lex(&val, &st);
    auto pos = current_pos();
    switch (tok) {
        case taOR:
        case taXOR:
        case taAND:
        case taADD:
        case taSUB:
        case taMULT:
        case taDIV:
        case taMOD:
        case taSHRU:
        case taSHR:
        case taSHL:
            break;
        case '=': /* simple assignment */
            if (sc_intest)
                error(211); /* possibly unintended assignment */
            break;
        default:
            lexpush();
            return node;
    }

    Expr* right = hier14();
    return new BinaryExpr(pos, tok, node, right);
}

Expr*
Parser::plnge(int* opstr, NewHierFn hier)
{
    int opidx;

    Expr* node = (this->*hier)();
    if (nextop(&opidx, opstr) == 0)
        return node;

    do {
        auto pos = current_pos();
        Expr* right = (this->*hier)();

        int token = opstr[opidx];
        switch (token) {
            case tlOR:
            case tlAND:
                node = new LogicalExpr(pos, token, node, right);
                break;
            default:
                node = new BinaryExpr(pos, token, node, right);
                break;
        }
    } while (nextop(&opidx, opstr));

    return node;
}

Expr*
Parser::plnge_rel(int* opstr, NewHierFn hier)
{
    int opidx;

    Expr* first = (this->*hier)();
    if (nextop(&opidx, opstr) == 0)
        return first;

    ChainedCompareExpr* chain = new ChainedCompareExpr(current_pos(), first);

    do {
        auto pos = current_pos();
        Expr* right = (this->*hier)();

        chain->ops().push_back(CompareOp(pos, opstr[opidx], right));
    } while (nextop(&opidx, opstr));

    return chain;
}

Expr*
Parser::hier13()
{
    Expr* node = hier12();
    if (matchtoken('?')) {
        auto pos = current_pos();
        Expr* left;
        {
            /* do not allow tagnames here (colon is a special token) */
            ke::SaveAndSet<bool> allowtags(&sc_allowtags, false);
            left = hier13();
        }
        needtoken(':');
        Expr* right = hier13();
        return new TernaryExpr(pos, node, left, right);
    }
    return node;
}

Expr*
Parser::hier12()
{
    return plnge(list12, &Parser::hier11);
}

Expr*
Parser::hier11()
{
    return plnge(list11, &Parser::hier10);
}

Expr*
Parser::hier10()
{
    return plnge(list10, &Parser::hier9);
}

Expr*
Parser::hier9()
{
    return plnge_rel(list9, &Parser::hier8);
}

Expr*
Parser::hier8()
{
    return plnge(list8, &Parser::hier7);
}

Expr*
Parser::hier7()
{
    return plnge(list7, &Parser::hier6);
}

Expr*
Parser::hier6()
{
    return plnge(list6, &Parser::hier5);
}

Expr*
Parser::hier5()
{
    return plnge(list5, &Parser::hier4);
}

Expr*
Parser::hier4()
{
    return plnge(list4, &Parser::hier3);
}

Expr*
Parser::hier3()
{
    return plnge(list3, &Parser::hier2);
}

Expr*
Parser::hier2()
{
    int val;
    char* st;
    int tok = lex(&val, &st);
    auto pos = current_pos();
    switch (tok) {
        case tINC: /* ++lval */
        case tDEC: /* --lval */
        {
            Expr* node = hier2();
            return new PreIncExpr(pos, tok, node);
        }
        case '~':
        case '-':
        case '!':
        {
            Expr* node = hier2();
            return new UnaryExpr(pos, tok, node);
        }
        case tNEW:
        {
            // :TODO: unify this to only care about types. This will depend on
            // removing immediate name resolution from parse_new_typename.
            token_ident_t ident;
            if (matchsymbol(&ident)) {
                if (matchtoken('(')) {
                    Expr* target = new SymbolExpr(current_pos(), gAtoms.add(ident.name));
                    return parse_call(pos, tok, target);
                }
                lexpush();
            }

            int tag = 0;
            parse_new_typename(nullptr, &tag);

            if (!needtoken('['))
                return new ErrorExpr();

            return parse_new_array(pos, tag);
        }
        case tLABEL: /* tagname override */
        {
            int tag = pc_addtag(st);
            if (sc_require_newdecls) {
                // Warn: old style cast used when newdecls pragma is enabled
                error(240, st, type_to_name(tag));
            }
            Expr* expr = hier2();
            return new CastExpr(pos, tok, tag, expr);
        }
        case tDEFINED:
        {
            int parens = 0;
            while (matchtoken('('))
                parens++;

            token_ident_t ident;
            if (!needsymbol(&ident))
                return new ErrorExpr();
            while (parens--)
                needtoken(')');
            return new IsDefinedExpr(pos, gAtoms.add(ident.name));
        }
        case tSIZEOF:
        {
            int parens = 0;
            while (matchtoken('('))
                parens++;

            token_ident_t ident;
            if (matchtoken(tTHIS)) {
                strcpy(ident.name, "this");
            } else {
                if (!needsymbol(&ident))
                    return new ErrorExpr();
            }

            int array_levels = 0;
            while (matchtoken('[')) {
                array_levels++;
                needtoken(']');
            }

            Atom* field = nullptr;
            int token = lex(&val, &st);
            if (token == tDBLCOLON || token == '.') {
                token_ident_t field_name;
                if (!needsymbol(&field_name))
                    return new ErrorExpr();
                field = gAtoms.add(field_name.name);
            } else {
                lexpush();
                token = 0;
            }

            while (parens--)
                needtoken(')');

            Atom* name = gAtoms.add(ident.name);
            return new SizeofExpr(pos, name, field, token, array_levels);
        }
        default:
            lexpush();
            break;
    }

    Expr* node = hier1();

    /* check for postfix operators */
    if (matchtoken(';')) {
        /* Found a ';', do not look further for postfix operators */
        lexpush(); /* push ';' back after successful match */
        return node;
    }
    if (matchtoken(tTERM)) {
        /* Found a newline that ends a statement (this is the case when
         * semicolons are optional). Note that an explicit semicolon was
         * handled above. This case is similar, except that the token must
         * not be pushed back.
         */
        return node;
    }

    tok = lex(&val, &st);
    switch (tok) {
        case tINC: /* lval++ */
        case tDEC: /* lval-- */
            return new PostIncExpr(current_pos(), tok, node);
        default:
            lexpush();
            break;
    }
    return node;
}

Expr*
Parser::hier1()
{
    Expr* base = nullptr;
    if (matchtoken(tVIEW_AS)) {
        base = parse_view_as();
    } else {
        base = primary();
    }

    for (;;) {
        char* st;
        cell val;
        int tok = lex(&val, &st);
        if (tok == '.' || tok == tDBLCOLON) {
            auto pos = current_pos();
            token_ident_t ident;
            if (!needsymbol(&ident))
                break;
            base = new FieldAccessExpr(pos, tok, base, gAtoms.add(ident.name));
        } else if (tok == '[') {
            auto pos = current_pos();
            Expr* inner = hier14();
            base = new IndexExpr(pos, base, inner);
            needtoken(']');
        } else if (tok == '(') {
            auto pos = current_pos();
            base = parse_call(pos, tok, base);
        } else {
            lexpush();
            break;
        }
    }
    return base;
}

Expr*
Parser::primary()
{
    if (matchtoken('(')) { /* sub-expression - (expression,...) */
        /* no longer in "test" expression */
        ke::SaveAndSet<bool> in_test(&sc_intest, false);
        /* allow tagnames to be used in parenthesized expressions */
        ke::SaveAndSet<bool> allowtags(&sc_allowtags, true);

        CommaExpr* expr = new CommaExpr(current_pos());
        do {
            Expr* child = hier14();
            expr->exprs().push_back(child);
        } while (matchtoken(','));
        needtoken(')');
        lexclr(FALSE); /* clear lex() push-back, it should have been
                        * cleared already by needtoken() */
        return expr;
    }

    cell val;
    char* st;
    int tok = lex(&val, &st);

    if (tok == tTHIS)
        return new ThisExpr(current_pos());
    if (tok == tSYMBOL)
        return new SymbolExpr(current_pos(), gAtoms.add(st));

    lexpush();

    return constant();
}

Expr*
Parser::constant()
{
    cell val;
    char* st;
    int tok = lex(&val, &st);
    auto pos = current_pos();
    switch (tok) {
        case tNULL:
            return new NullExpr(pos);
        case tNUMBER:
            return new NumberExpr(pos, val);
        case tRATIONAL:
            return new FloatExpr(pos, val);
        case tSTRING:
            return new StringExpr(pos, current_token()->str, current_token()->len);
        case '{':
        {
            ArrayExpr* expr = new ArrayExpr(pos);
            do {
                if (matchtoken(tELLIPS)) {
                    expr->set_ellipses();
                    break;
                }
                Expr* child = hier14();
                expr->exprs().push_back(child);
            } while (matchtoken(','));
            if (!needtoken('}'))
                lexclr(FALSE);
            return expr;
        }
        default:
          error(29);
          return new ErrorExpr();
    }
}

CallExpr*
Parser::parse_call(const token_pos_t& pos, int tok, Expr* target)
{
    CallExpr* call = new CallExpr(pos, tok, target);

    if (matchtoken(')'))
        return call;

    bool named_params = false;
    do {
        sp::Atom* name = nullptr;
        if (matchtoken('.')) {
            named_params = true;

            token_ident_t ident;
            if (!needsymbol(&ident))
                break;
            needtoken('=');

            name = gAtoms.add(ident.name);
        } else {
            if (named_params)
                error(44);
        }

        Expr* expr = nullptr;
        if (!matchtoken('_'))
            expr = hier14();

        call->args().emplace_back(name, expr);

        if (matchtoken(')'))
            break;
        if (!needtoken(','))
            break;
    } while (freading && !matchtoken(tENDEXPR));

    return call;
}

Expr*
Parser::parse_view_as()
{
    auto pos = current_pos();

    needtoken('<');
    int tag = 0;
    {
        token_t tok;
        lextok(&tok);
        if (!parse_new_typename(&tok, &tag))
            tag = 0;
    }
    needtoken('>');

    int paren = needtoken('(');

    Expr* expr = hier14();
    if (paren)
        needtoken(')');
    else
        matchtoken(')');
    return new CastExpr(pos, tVIEW_AS, tag, expr);
}

Expr*
Parser::struct_init()
{
    StructExpr* init = new StructExpr(current_pos());

    // '}' has already been lexed.
    do {
        sp::Atom* name = nullptr;

        token_ident_t ident;
        if (needsymbol(&ident))
            name = gAtoms.add(ident.name);

        needtoken('=');

        auto pos = current_pos();

        cell value;
        char* str;
        Expr* expr = nullptr;
        switch (lex(&value, &str)) {
            case tSTRING:
                expr = new StringExpr(pos, current_token()->str, current_token()->len);
                break;
            case tNUMBER:
                expr = new NumberExpr(pos, value);
                break;
            case tRATIONAL:
                expr = new FloatExpr(pos, value);
                break;
            default:
                error(1, "-constant-", str);
                break;
        }

        if (name && expr)
            init->fields().push_back(StructInitField(name, expr));
    } while (matchtoken(',') && !lexpeek('}'));

    needtoken('}');
    return init;
}

Stmt*
Parser::parse_static_assert()
{
    auto pos = current_pos();

    needtoken('(');

    int expr_val, expr_tag;
    bool is_const = exprconst(&expr_val, &expr_tag, nullptr);

    PoolString * text = nullptr;
    if (matchtoken(',') && needtoken(tSTRING)) {
        auto tok = current_token();
        text = new PoolString(tok->str, tok->len);
    }

    needtoken(')');
    require_newline(TerminatorPolicy::NewlineOrSemicolon);

    if (!is_const)
        return nullptr;

    return new StaticAssertStmt(pos, expr_val, text);
}

Expr*
Parser::var_init(int vclass)
{
    if (matchtoken('{')) {
        ArrayExpr* expr = new ArrayExpr(current_pos());
        do {
            if (lexpeek('}'))
                break;
            if (matchtoken(tELLIPS)) {
                expr->set_ellipses();
                break;
            }
            Expr* child = var_init(vclass);
            expr->exprs().emplace_back(child);
        } while (matchtoken(','));
        needtoken('}');
        return expr;
    }

    if (matchtoken(tSTRING)) {
        auto tok = current_token();
        return new StringExpr(tok->start, tok->str, tok->len);
    }

    // We'll check const or symbol-ness for non-sLOCALs in the semantic pass.
    return hier14();
}

Expr*
Parser::parse_new_array(const token_pos_t& pos, int tag)
{
    auto expr = new NewArrayExpr(pos, tag);

    do {
        Expr* child = hier14();
        expr->exprs().emplace_back(child);

        needtoken(']');
    } while (matchtoken('['));
    return expr;
}

void
Parser::parse_post_dims(typeinfo_t* type)
{
    Expr* old_dims[sDIMEN_MAX];
    bool has_old_dims = false;

    do {
        if (type->numdim == sDIMEN_MAX) {
            error(53);
            break;
        }

        type->idxtag[type->numdim] = 0;
        type->dim[type->numdim] = 0;

        if (matchtoken(']')) {
            old_dims[type->numdim] = nullptr;
        } else {
            old_dims[type->numdim] = hier14();
            has_old_dims = true;
            needtoken(']');
        }
        type->numdim++;
    } while (matchtoken('['));

    if (has_old_dims) {
        type->dim_exprs = gPoolAllocator.alloc<Expr*>(type->numdim);
        memcpy(type->dim_exprs, old_dims, sizeof(Expr*) * type->numdim);
    }
}

Stmt*
Parser::parse_stmt(int* lastindent, bool allow_decl)
{
    // :TODO: remove this when compound goes private
    ke::SaveAndSet<bool> limit_errors(&sc_one_error_per_statement, true);

    if (!freading) {
        error(36); /* empty statement */
        return nullptr;
    }
    errorset(sRESET, 0);

    cell val;
    char* st;
    int tok = lex(&val, &st);
    if (tok != '{') {
        insert_dbgline(fline);
        setline(TRUE);
    }

    /* lex() has set stmtindent */
    if (lastindent && tok != tLABEL) {
        if (*lastindent >= 0 && *lastindent != stmtindent && !indent_nowarn && sc_tabsize > 0)
            error(217); /* loose indentation */
        *lastindent = stmtindent;
        indent_nowarn = FALSE; /* if warning was blocked, re-enable it */
    }

    if (tok == tSYMBOL) {
        // We reaaaally don't have enough lookahead for this, so we cheat and try
        // to determine whether this is probably a declaration.
        int is_decl = FALSE;
        if (matchtoken('[')) {
            if (lexpeek(']'))
                is_decl = TRUE;
            lexpush();
        } else if (lexpeek(tSYMBOL)) {
            is_decl = TRUE;
        }

        if (is_decl) {
            if (!allow_decl) {
                error(3);
                return nullptr;
            }
            lexpush();
            return parse_local_decl(tNEWDECL, true);
        }
    }

    switch (tok) {
        case 0:
            /* nothing */
            return nullptr;
        case tINT:
        case tVOID:
        case tCHAR:
        case tOBJECT:
            lexpush();
            // Fall-through.
        case tDECL:
        case tSTATIC:
        case tNEW:
            if (tok == tNEW && matchtoken(tSYMBOL)) {
                if (lexpeek('(')) {
                    lexpush();
                    break;
                }
                lexpush(); // we matchtoken'ed, give it back to lex for declloc
            }
            if (!allow_decl) {
                error(3);
                return nullptr;
            }
            return parse_local_decl(tok, tok != tDECL);
        case tIF:
            return parse_if();
        case tCONST:
            return parse_const(sLOCAL);
        case tENUM:
            return parse_enum(sLOCAL);
        case tCASE:
        case tDEFAULT:
            error(14); /* not in switch */
            return nullptr;
        case '{': {
            int save = fline;
            if (matchtoken('}'))
                return new StmtList(current_pos());
            return parse_compound(save == fline);
        }
        case ';':
            error(36); /* empty statement */
            return nullptr;
        case tBREAK:
        case tCONTINUE: {
            auto pos = current_pos();
            needtoken(tTERM);
            if (!in_loop_) {
                error(24);
                return nullptr;
            }
            return new LoopControlStmt(pos, tok);
        }
        case tRETURN: {
            auto pos = current_pos();
            Expr* expr = nullptr;
            if (!matchtoken(tTERM)) {
                expr = hier14();
                needtoken(tTERM);
            }
            return new ReturnStmt(pos, expr);
        }
        case tASSERT: {
            auto pos = current_pos();
            Expr* expr = parse_expr(true);
            needtoken(tTERM);
            if (!expr)
                return nullptr;
            return new AssertStmt(pos, expr);
        }
        case tDELETE: {
            auto pos = current_pos();
            Expr* expr = parse_expr(false);
            needtoken(tTERM);
            if (!expr)
                return nullptr;
            return new DeleteStmt(pos, expr);
        }
        case tEXIT: {
            auto pos = current_pos();
            Expr* expr = nullptr;
            if (matchtoken(tTERM)) {
                expr = parse_expr(false);
                needtoken(tTERM);
            }
            return new ExitStmt(pos, expr);
        }
        case tDO: {
            auto pos = current_pos();
            Stmt* stmt = nullptr;
            {
                ke::SaveAndSet<bool> in_loop(&in_loop_, true);
                stmt = parse_stmt(nullptr, false);
            }
            needtoken(tWHILE);
            bool parens = matchtoken('(');
            Expr* cond = parse_expr(false);
            if (parens)
                needtoken(')');
            else
                error(243);
            needtoken(tTERM);
            if (!stmt || !cond)
                return nullptr;
            return new DoWhileStmt(pos, tok, cond, stmt);
        }
        case tWHILE: {
            auto pos = current_pos();
            Expr* cond = parse_expr(true);
            Stmt* stmt = nullptr;
            {
                ke::SaveAndSet<bool> in_loop(&in_loop_, true);
                stmt = parse_stmt(nullptr, false);
            }
            if (!stmt || !cond)
                return nullptr;
            return new DoWhileStmt(pos, tok, cond, stmt);
        }
        case tFOR:
            return parse_for();
        case tSWITCH:
            return parse_switch();
        default: /* non-empty expression */
            break;
    }

    lexpush(); /* analyze token later */
    Expr* expr = parse_expr(false);
    needtoken(tTERM);
    if (!expr)
        return nullptr;
    return new ExprStmt(expr->pos(), expr);
}

Stmt*
Parser::parse_compound(bool sameline)
{
    auto block_start = fline;

    BlockStmt* block = new BlockStmt(current_pos());

    /* if there is more text on this line, we should adjust the statement indent */
    if (sameline) {
        int i;
        const unsigned char* p = lptr;
        /* go back to the opening brace */
        while (*p != '{') {
            assert(p > pline);
            p--;
        }
        assert(*p == '{'); /* it should be found */
        /* go forward, skipping white-space */
        p++;
        while (*p <= ' ' && *p != '\0')
            p++;
        assert(*p != '\0'); /* a token should be found */
        stmtindent = 0;
        for (i = 0; i < (int)(p - pline); i++)
            if (pline[i] == '\t' && sc_tabsize > 0)
                stmtindent += (int)(sc_tabsize - (stmtindent + sc_tabsize) % sc_tabsize);
            else
                stmtindent++;
    }

    int indent = -1;
    while (matchtoken('}') == 0) { /* repeat until compound statement is closed */
        if (!freading) {
            error(30, block_start); /* compound block not closed at end of file */
            break;
        }
        if (Stmt* stmt = parse_stmt(&indent, true))
            block->stmts().push_back(stmt);
    }

    return block;
}

Stmt*
Parser::parse_local_decl(int tokid, bool autozero)
{
    declinfo_t decl = {};

    int declflags = DECLFLAG_VARIABLE | DECLFLAG_ENUMROOT;
    if (tokid == tNEW || tokid == tDECL)
        declflags |= DECLFLAG_OLD;
    else if (tokid == tNEWDECL)
        declflags |= DECLFLAG_NEW;

    parse_decl(&decl, declflags);

    Parser::VarParams params;
    params.vclass = (tokid == tSTATIC) ? sSTATIC : sLOCAL;
    params.autozero = autozero;
    return parse_var(&decl, params);
}

Stmt*
Parser::parse_if()
{
    auto ifindent = stmtindent;
    auto pos = current_pos();
    auto expr = parse_expr(true);
    if (!expr)
        return nullptr;
    auto stmt = parse_stmt(nullptr, false);
    Stmt* else_stmt = nullptr;
    if (matchtoken(tELSE)) {
        /* to avoid the "dangling else" error, we want a warning if the "else"
         * has a lower indent than the matching "if" */
        if (stmtindent < ifindent && sc_tabsize > 0)
            error(217); /* loose indentation */
        else_stmt = parse_stmt(nullptr, false);
        if (!else_stmt)
            return nullptr;
    }
    if (!stmt)
        return nullptr;
    return new IfStmt(pos, expr, stmt, else_stmt);
}

Expr*
Parser::parse_expr(bool parens)
{
    ke::SaveAndSet<bool> in_test(&sc_intest, parens);

    if (parens)
        needtoken('(');

    Expr* expr = nullptr;
    CommaExpr* comma = nullptr;
    while (true) {
        expr = hier14();
        if (!expr)
            break;

        if (comma)
            comma->exprs().push_back(expr);

        if (!matchtoken(','))
            break;

        if (!comma) {
            comma = new CommaExpr(expr->pos());
            comma->exprs().push_back(expr);
        }
    }
    if (parens)
        needtoken(')');

    return comma ? comma : expr;
}

Stmt*
Parser::parse_for()
{
    auto pos = current_pos();

    int endtok = matchtoken('(') ? ')' : tDO;
    if (endtok != ')')
        error(243);

    Stmt* init = nullptr;
    if (!matchtoken(';')) {
        /* new variable declarations are allowed here */
        token_t tok;

        switch (lextok(&tok)) {
            case tINT:
            case tCHAR:
            case tOBJECT:
            case tVOID:
                lexpush();
                // Fallthrough.
            case tNEW:
                /* The variable in expr1 of the for loop is at a
                 * 'compound statement' level of it own.
                 */
                // :TODO: test needtoken(tTERM) accepting newlines here
                init = parse_local_decl(tok.id, true);
                break;
            case tSYMBOL: {
                // See comment in statement() near tSYMBOL.
                bool is_decl = false;
                if (matchtoken('[')) {
                    if (lexpeek(']'))
                        is_decl = true;
                    lexpush();
                } else if (lexpeek(tSYMBOL)) {
                    is_decl = true;
                }

                if (is_decl) {
                    lexpush();
                    init = parse_local_decl(tSYMBOL, true);
                    break;
                }
                // Fall-through to default!
            }
            default:
                lexpush();
                if (Expr* expr = parse_expr(false))
                    init = new ExprStmt(expr->pos(), expr);
                needtoken(';');
                break;
        }
    }

    Expr* cond = nullptr;
    if (!matchtoken(';')) {
        cond = parse_expr(false);
        needtoken(';');
    }

    Expr* advance = nullptr;
    if (!matchtoken(endtok)) {
        advance = parse_expr(false);
        needtoken(endtok);
    }

    Stmt* body = nullptr;
    {
        ke::SaveAndSet<bool> in_loop(&in_loop_, true);
        body = parse_stmt(nullptr, false);
    }
    if (!body)
        return nullptr;
    return new ForStmt(pos, init, cond, advance, body);
}

Stmt*
Parser::parse_switch()
{
    auto pos = current_pos();

    int endtok = matchtoken('(') ? ')' : tDO;
    if (endtok != ')')
        error(243);

    Expr* cond = parse_expr(false);
    needtoken(endtok);

    SwitchStmt* sw = new SwitchStmt(pos, cond);

    endtok = '}';
    needtoken('{');
    while (true) {
        cell val;
        char* st;
        int tok = lex(&val, &st);

        switch (tok) {
            case tCASE:
                if (sw->default_case())
                    error(15); /* "default" case must be last in switch statement */
                parse_case(sw);
                break;
            case tDEFAULT:
                needtoken(':');
                if (Stmt* stmt = parse_stmt(nullptr, false)) {
                    if (sw->default_case())
                        error(16);
                    else
                        sw->set_default_case(stmt);
                }
                break;
            default:
                if (tok != '}') {
                    error(2);
                    indent_nowarn = TRUE;
                    tok = endtok;
                }
                break;
        }
        if (tok == endtok)
            break;
    }

    if (!cond)
        return nullptr;

    return sw;
}

void
Parser::parse_case(SwitchStmt* sw)
{
    PoolList<Expr*> exprs;
    do {
        /* do not allow tagnames here */
        ke::SaveAndSet<bool> allowtags(&sc_allowtags, false);

        // hier14 because parse_expr() allows comma exprs
        if (Expr* expr = hier14())
            exprs.push_back(expr);
        if (matchtoken(tDBLDOT))
            error(1, ":", "..");
    } while (matchtoken(','));

    needtoken(':');

    Stmt* stmt = parse_stmt(nullptr, false);
    if (!stmt || exprs.empty())
        return;

    sw->AddCase(std::move(exprs), stmt);
}
