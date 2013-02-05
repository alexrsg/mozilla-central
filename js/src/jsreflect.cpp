/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS reflection package.
 */
#include <stdlib.h>

#include "mozilla/Util.h"

#include "jspubtd.h"
#include "jsatom.h"
#include "jsobj.h"
#include "jsreflect.h"
#include "jsprf.h"
#include "jsiter.h"
#include "jsbool.h"
#include "jsval.h"
#include "jsinferinlines.h"
#include "jsobjinlines.h"
#include "jsarray.h"
#include "jsnum.h"

#include "frontend/Parser.h"
#include "frontend/TokenStream.h"
#include "vm/RegExpObject.h"

#include "jsscriptinlines.h"

using namespace js;
using namespace js::frontend;

using mozilla::DebugOnly;
using mozilla::ArrayLength;

namespace js {

char const *aopNames[] = {
    "=",    /* AOP_ASSIGN */
    "+=",   /* AOP_PLUS */
    "-=",   /* AOP_MINUS */
    "*=",   /* AOP_STAR */
    "/=",   /* AOP_DIV */
    "%=",   /* AOP_MOD */
    "<<=",  /* AOP_LSH */
    ">>=",  /* AOP_RSH */
    ">>>=", /* AOP_URSH */
    "|=",   /* AOP_BITOR */
    "^=",   /* AOP_BITXOR */
    "&="    /* AOP_BITAND */
};

char const *binopNames[] = {
    "==",         /* BINOP_EQ */
    "!=",         /* BINOP_NE */
    "===",        /* BINOP_STRICTEQ */
    "!==",        /* BINOP_STRICTNE */
    "<",          /* BINOP_LT */
    "<=",         /* BINOP_LE */
    ">",          /* BINOP_GT */
    ">=",         /* BINOP_GE */
    "<<",         /* BINOP_LSH */
    ">>",         /* BINOP_RSH */
    ">>>",        /* BINOP_URSH */
    "+",          /* BINOP_PLUS */
    "-",          /* BINOP_MINUS */
    "*",          /* BINOP_STAR */
    "/",          /* BINOP_DIV */
    "%",          /* BINOP_MOD */
    "|",          /* BINOP_BITOR */
    "^",          /* BINOP_BITXOR */
    "&",          /* BINOP_BITAND */
    "in",         /* BINOP_IN */
    "instanceof", /* BINOP_INSTANCEOF */
    "..",         /* BINOP_DBLDOT */
};

char const *unopNames[] = {
    "delete",  /* UNOP_DELETE */
    "-",       /* UNOP_NEG */
    "+",       /* UNOP_POS */
    "!",       /* UNOP_NOT */
    "~",       /* UNOP_BITNOT */
    "typeof",  /* UNOP_TYPEOF */
    "void"     /* UNOP_VOID */
};

char const *nodeTypeNames[] = {
#define ASTDEF(ast, str, method) str,
#include "jsast.tbl"
#undef ASTDEF
    NULL
};

char const *callbackNames[] = {
#define ASTDEF(ast, str, method) method,
#include "jsast.tbl"
#undef ASTDEF
    NULL
};

typedef AutoValueVector NodeVector;

/*
 * ParseNode is a somewhat intricate data structure, and its invariants have
 * evolved, making it more likely that there could be a disconnect between the
 * parser and the AST serializer. We use these macros to check invariants on a
 * parse node and raise a dynamic error on failure.
 */
#define LOCAL_ASSERT(expr)                                                             \
    JS_BEGIN_MACRO                                                                     \
        JS_ASSERT(expr);                                                               \
        if (!(expr)) {                                                                 \
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_PARSE_NODE);  \
            return false;                                                              \
        }                                                                              \
    JS_END_MACRO

#define LOCAL_NOT_REACHED(expr)                                                        \
    JS_BEGIN_MACRO                                                                     \
        JS_NOT_REACHED(expr);                                                          \
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_PARSE_NODE);      \
        return false;                                                                  \
    JS_END_MACRO

/*
 * Builder class that constructs JavaScript AST node objects. See:
 *
 *     https://developer.mozilla.org/en/SpiderMonkey/Parser_API
 *
 * Bug 569487: generalize builder interface
 */
class NodeBuilder
{
    JSContext   *cx;
    bool        saveLoc;               /* save source location information?     */
    char const  *src;                  /* source filename or null               */
    RootedValue srcval;                /* source filename JS value or null      */
    Value       callbacks[AST_LIMIT];  /* user-specified callbacks              */
    AutoValueArray callbacksRoots;     /* for rooting |callbacks|               */
    RootedValue userv;                 /* user-specified builder object or null */
    RootedValue undefinedVal;          /* a rooted undefined val, used by opt() */

  public:
    NodeBuilder(JSContext *c, bool l, char const *s)
        : cx(c), saveLoc(l), src(s), srcval(c),
          callbacksRoots(c, callbacks, AST_LIMIT), userv(c), undefinedVal(c, UndefinedValue())
    {
        MakeRangeGCSafe(callbacks, mozilla::ArrayLength(callbacks));
    }

    bool init(HandleObject userobj = NullPtr()) {
        if (src) {
            if (!atomValue(src, &srcval))
                return false;
        } else {
            srcval.setNull();
        }

        if (!userobj) {
            userv.setNull();
            for (unsigned i = 0; i < AST_LIMIT; i++) {
                callbacks[i].setNull();
            }
            return true;
        }

        userv.setObject(*userobj);

        RootedValue nullVal(cx, NullValue());
        RootedValue funv(cx);
        for (unsigned i = 0; i < AST_LIMIT; i++) {
            const char *name = callbackNames[i];
            RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
            if (!atom)
                return false;
            RootedId id(cx, AtomToId(atom));
            if (!baseops::GetPropertyDefault(cx, userobj, id, nullVal, &funv))
                return false;

            if (funv.isNullOrUndefined()) {
                callbacks[i].setNull();
                continue;
            }

            if (!funv.isObject() || !funv.toObject().isFunction()) {
                js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NOT_FUNCTION,
                                         JSDVG_SEARCH_STACK, funv, NullPtr(), NULL, NULL);
                return false;
            }

            callbacks[i] = funv;
        }

        return true;
    }

  private:
    bool callback(HandleValue fun, TokenPos *pos, MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { loc };
            AutoValueArray ava(cx, argv, 1);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { NullValue() }; /* no zero-length arrays allowed! */
        AutoValueArray ava(cx, argv, 1);
        return Invoke(cx, userv, fun, 0, argv, dst.address());
    }

    bool callback(HandleValue fun, HandleValue v1, TokenPos *pos, MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { v1, loc };
            AutoValueArray ava(cx, argv, 2);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { v1 };
        AutoValueArray ava(cx, argv, 1);
        return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
    }

    bool callback(HandleValue fun, HandleValue v1, HandleValue v2, TokenPos *pos,
                  MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { v1, v2, loc };
            AutoValueArray ava(cx, argv, 3);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { v1, v2 };
        AutoValueArray ava(cx, argv, 2);
        return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
    }

    bool callback(HandleValue fun, HandleValue v1, HandleValue v2, HandleValue v3, TokenPos *pos,
                  MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { v1, v2, v3, loc };
            AutoValueArray ava(cx, argv, 4);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { v1, v2, v3 };
        AutoValueArray ava(cx, argv, 3);
        return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
    }

    bool callback(HandleValue fun, HandleValue v1, HandleValue v2, HandleValue v3, HandleValue v4,
                  TokenPos *pos, MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { v1, v2, v3, v4, loc };
            AutoValueArray ava(cx, argv, 5);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { v1, v2, v3, v4 };
        AutoValueArray ava(cx, argv, 4);
        return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
    }

    bool callback(HandleValue fun, HandleValue v1, HandleValue v2, HandleValue v3, HandleValue v4,
                  HandleValue v5, TokenPos *pos, MutableHandleValue dst) {
        if (saveLoc) {
            RootedValue loc(cx);
            if (!newNodeLoc(pos, &loc))
                return false;
            Value argv[] = { v1, v2, v3, v4, v5, loc };
            AutoValueArray ava(cx, argv, 6);
            return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
        }

        Value argv[] = { v1, v2, v3, v4, v5 };
        AutoValueArray ava(cx, argv, 5);
        return Invoke(cx, userv, fun, ArrayLength(argv), argv, dst.address());
    }

    // WARNING: Returning a Handle is non-standard, but it works in this case
    // because both |v| and |undefinedVal| are definitely rooted on a previous
    // stack frame (i.e. we're just choosing between two already-rooted
    // values).
    HandleValue opt(HandleValue v) {
        JS_ASSERT_IF(v.isMagic(), v.whyMagic() == JS_SERIALIZE_NO_NODE);
        return v.isMagic(JS_SERIALIZE_NO_NODE) ? undefinedVal : v;
    }

    bool atomValue(const char *s, MutableHandleValue dst) {
        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, s, strlen(s)));
        if (!atom)
            return false;

        dst.setString(atom);
        return true;
    }

    bool newObject(MutableHandleObject dst) {
        RootedObject nobj(cx, NewBuiltinClassInstance(cx, &ObjectClass));
        if (!nobj)
            return false;

        dst.set(nobj);
        return true;
    }

    bool newArray(NodeVector &elts, MutableHandleValue dst);

    bool newNode(ASTType type, TokenPos *pos, MutableHandleObject dst);

    bool newNode(ASTType type, TokenPos *pos, MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName, HandleValue child,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName, child) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName1, HandleValue child1,
                 const char *childName2, HandleValue child2,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName1, child1) &&
               setProperty(node, childName2, child2) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName1, HandleValue child1,
                 const char *childName2, HandleValue child2,
                 const char *childName3, HandleValue child3,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName1, child1) &&
               setProperty(node, childName2, child2) &&
               setProperty(node, childName3, child3) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName1, HandleValue child1,
                 const char *childName2, HandleValue child2,
                 const char *childName3, HandleValue child3,
                 const char *childName4, HandleValue child4,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName1, child1) &&
               setProperty(node, childName2, child2) &&
               setProperty(node, childName3, child3) &&
               setProperty(node, childName4, child4) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName1, HandleValue child1,
                 const char *childName2, HandleValue child2,
                 const char *childName3, HandleValue child3,
                 const char *childName4, HandleValue child4,
                 const char *childName5, HandleValue child5,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName1, child1) &&
               setProperty(node, childName2, child2) &&
               setProperty(node, childName3, child3) &&
               setProperty(node, childName4, child4) &&
               setProperty(node, childName5, child5) &&
               setResult(node, dst);
    }

    bool newNode(ASTType type, TokenPos *pos,
                 const char *childName1, HandleValue child1,
                 const char *childName2, HandleValue child2,
                 const char *childName3, HandleValue child3,
                 const char *childName4, HandleValue child4,
                 const char *childName5, HandleValue child5,
                 const char *childName6, HandleValue child6,
                 const char *childName7, HandleValue child7,
                 MutableHandleValue dst) {
        RootedObject node(cx);
        return newNode(type, pos, &node) &&
               setProperty(node, childName1, child1) &&
               setProperty(node, childName2, child2) &&
               setProperty(node, childName3, child3) &&
               setProperty(node, childName4, child4) &&
               setProperty(node, childName5, child5) &&
               setProperty(node, childName6, child6) &&
               setProperty(node, childName7, child7) &&
               setResult(node, dst);
    }

    bool listNode(ASTType type, const char *propName, NodeVector &elts, TokenPos *pos,
                  MutableHandleValue dst) {
        RootedValue array(cx);
        if (!newArray(elts, &array))
            return false;

        RootedValue cb(cx, callbacks[type]);
        if (!cb.isNull())
            return callback(cb, array, pos, dst);

        return newNode(type, pos, propName, array, dst);
    }

    bool setProperty(HandleObject obj, const char *name, HandleValue val) {
        JS_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

        /*
         * Bug 575416: instead of Atomize, lookup constant atoms in tbl file
         */
        RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
        if (!atom)
            return false;

        /* Represent "no node" as null and ensure users are not exposed to magic values. */
        RootedValue optVal(cx, val.isMagic(JS_SERIALIZE_NO_NODE) ? NullValue() : val);
        return JSObject::defineProperty(cx, obj, atom->asPropertyName(), optVal);
    }

    bool newNodeLoc(TokenPos *pos, MutableHandleValue dst);

    bool setNodeLoc(HandleObject node, TokenPos *pos);

    bool setResult(HandleObject obj, MutableHandleValue dst) {
        JS_ASSERT(obj);
        dst.setObject(*obj);
        return true;
    }

  public:
    /*
     * All of the public builder methods take as their last two
     * arguments a nullable token position and a non-nullable, rooted
     * outparam.
     *
     * Any Value arguments representing optional subnodes may be a
     * JS_SERIALIZE_NO_NODE magic value.
     */

    /*
     * misc nodes
     */

    bool program(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool literal(HandleValue val, TokenPos *pos, MutableHandleValue dst);

    bool identifier(HandleValue name, TokenPos *pos, MutableHandleValue dst);

    bool function(ASTType type, TokenPos *pos,
                  HandleValue id, NodeVector &args, NodeVector &defaults,
                  HandleValue body, HandleValue rest, bool isGenerator, bool isExpression,
                  MutableHandleValue dst);

    bool variableDeclarator(HandleValue id, HandleValue init, TokenPos *pos,
                            MutableHandleValue dst);

    bool switchCase(HandleValue expr, NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool catchClause(HandleValue var, HandleValue guard, HandleValue body, TokenPos *pos,
                     MutableHandleValue dst);

    bool propertyInitializer(HandleValue key, HandleValue val, PropKind kind, TokenPos *pos,
                             MutableHandleValue dst);


    /*
     * statements
     */

    bool blockStatement(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool expressionStatement(HandleValue expr, TokenPos *pos, MutableHandleValue dst);

    bool emptyStatement(TokenPos *pos, MutableHandleValue dst);

    bool ifStatement(HandleValue test, HandleValue cons, HandleValue alt, TokenPos *pos,
                     MutableHandleValue dst);

    bool breakStatement(HandleValue label, TokenPos *pos, MutableHandleValue dst);

    bool continueStatement(HandleValue label, TokenPos *pos, MutableHandleValue dst);

    bool labeledStatement(HandleValue label, HandleValue stmt, TokenPos *pos,
                          MutableHandleValue dst);

    bool throwStatement(HandleValue arg, TokenPos *pos, MutableHandleValue dst);

    bool returnStatement(HandleValue arg, TokenPos *pos, MutableHandleValue dst);

    bool forStatement(HandleValue init, HandleValue test, HandleValue update, HandleValue stmt,
                      TokenPos *pos, MutableHandleValue dst);

    bool forInStatement(HandleValue var, HandleValue expr, HandleValue stmt,
                        bool isForEach, TokenPos *pos, MutableHandleValue dst);

    bool forOfStatement(HandleValue var, HandleValue expr, HandleValue stmt, TokenPos *pos,
                        MutableHandleValue dst);

    bool withStatement(HandleValue expr, HandleValue stmt, TokenPos *pos, MutableHandleValue dst);

    bool whileStatement(HandleValue test, HandleValue stmt, TokenPos *pos, MutableHandleValue dst);

    bool doWhileStatement(HandleValue stmt, HandleValue test, TokenPos *pos,
                          MutableHandleValue dst);

    bool switchStatement(HandleValue disc, NodeVector &elts, bool lexical, TokenPos *pos,
                         MutableHandleValue dst);

    bool tryStatement(HandleValue body, NodeVector &guarded, HandleValue unguarded,
                      HandleValue finally, TokenPos *pos, MutableHandleValue dst);

    bool debuggerStatement(TokenPos *pos, MutableHandleValue dst);

    bool letStatement(NodeVector &head, HandleValue stmt, TokenPos *pos, MutableHandleValue dst);

    /*
     * expressions
     */

    bool binaryExpression(BinaryOperator op, HandleValue left, HandleValue right, TokenPos *pos,
                          MutableHandleValue dst);

    bool unaryExpression(UnaryOperator op, HandleValue expr, TokenPos *pos, MutableHandleValue dst);

    bool assignmentExpression(AssignmentOperator op, HandleValue lhs, HandleValue rhs,
                              TokenPos *pos, MutableHandleValue dst);

    bool updateExpression(HandleValue expr, bool incr, bool prefix, TokenPos *pos,
                          MutableHandleValue dst);

    bool logicalExpression(bool lor, HandleValue left, HandleValue right, TokenPos *pos,
                           MutableHandleValue dst);

    bool conditionalExpression(HandleValue test, HandleValue cons, HandleValue alt, TokenPos *pos,
                               MutableHandleValue dst);

    bool sequenceExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool newExpression(HandleValue callee, NodeVector &args, TokenPos *pos, MutableHandleValue dst);

    bool callExpression(HandleValue callee, NodeVector &args, TokenPos *pos,
                        MutableHandleValue dst);

    bool memberExpression(bool computed, HandleValue expr, HandleValue member, TokenPos *pos,
                          MutableHandleValue dst);

    bool arrayExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool spreadExpression(HandleValue expr, TokenPos *pos, MutableHandleValue dst);

    bool objectExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool thisExpression(TokenPos *pos, MutableHandleValue dst);

    bool yieldExpression(HandleValue arg, TokenPos *pos, MutableHandleValue dst);

    bool comprehensionBlock(HandleValue patt, HandleValue src, bool isForEach, TokenPos *pos,
                            MutableHandleValue dst);

    bool comprehensionExpression(HandleValue body, NodeVector &blocks, HandleValue filter,
                                 TokenPos *pos, MutableHandleValue dst);

    bool generatorExpression(HandleValue body, NodeVector &blocks, HandleValue filter,
                             TokenPos *pos, MutableHandleValue dst);

    bool letExpression(NodeVector &head, HandleValue expr, TokenPos *pos, MutableHandleValue dst);

    /*
     * declarations
     */

    bool variableDeclaration(NodeVector &elts, VarDeclKind kind, TokenPos *pos,
                             MutableHandleValue dst);

    /*
     * patterns
     */

    bool arrayPattern(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool objectPattern(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool propertyPattern(HandleValue key, HandleValue patt, TokenPos *pos, MutableHandleValue dst);

    /*
     * xml
     */

    bool xmlAnyName(TokenPos *pos, MutableHandleValue dst);

    bool xmlEscapeExpression(HandleValue expr, TokenPos *pos, MutableHandleValue dst);

    bool xmlDefaultNamespace(HandleValue ns, TokenPos *pos, MutableHandleValue dst);

    bool xmlFilterExpression(HandleValue left, HandleValue right, TokenPos *pos,
                             MutableHandleValue dst);

    bool xmlAttributeSelector(HandleValue expr, bool computed, TokenPos *pos,
                              MutableHandleValue dst);

    bool xmlQualifiedIdentifier(HandleValue left, HandleValue right, bool computed, TokenPos *pos,
                                MutableHandleValue dst);

    bool xmlFunctionQualifiedIdentifier(HandleValue right, bool computed, TokenPos *pos,
                                        MutableHandleValue dst);

    bool xmlElement(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlText(HandleValue text, TokenPos *pos, MutableHandleValue dst);

    bool xmlList(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlStartTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlEndTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlPointTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlName(HandleValue text, TokenPos *pos, MutableHandleValue dst);

    bool xmlName(NodeVector &elts, TokenPos *pos, MutableHandleValue dst);

    bool xmlAttribute(HandleValue text, TokenPos *pos, MutableHandleValue dst);

    bool xmlCdata(HandleValue text, TokenPos *pos, MutableHandleValue dst);

    bool xmlComment(HandleValue text, TokenPos *pos, MutableHandleValue dst);

    bool xmlPI(HandleValue target, HandleValue content, TokenPos *pos, MutableHandleValue dst);
};

bool
NodeBuilder::newNode(ASTType type, TokenPos *pos, MutableHandleObject dst)
{
    JS_ASSERT(type > AST_ERROR && type < AST_LIMIT);

    RootedValue tv(cx);
    RootedObject node(cx, NewBuiltinClassInstance(cx, &ObjectClass));
    if (!node ||
        !setNodeLoc(node, pos) ||
        !atomValue(nodeTypeNames[type], &tv) ||
        !setProperty(node, "type", tv)) {
        return false;
    }

    dst.set(node);
    return true;
}

bool
NodeBuilder::newArray(NodeVector &elts, MutableHandleValue dst)
{
    const size_t len = elts.length();
    if (len > UINT32_MAX) {
        js_ReportAllocationOverflow(cx);
        return false;
    }
    RootedObject array(cx, NewDenseAllocatedArray(cx, uint32_t(len)));
    if (!array)
        return false;

    for (size_t i = 0; i < len; i++) {
        RootedValue val(cx, elts[i]);

        JS_ASSERT_IF(val.isMagic(), val.whyMagic() == JS_SERIALIZE_NO_NODE);

        /* Represent "no node" as an array hole by not adding the value. */
        if (val.isMagic(JS_SERIALIZE_NO_NODE))
            continue;

        if (!JSObject::setElement(cx, array, array, i, &val, false))
            return false;
    }

    dst.setObject(*array);
    return true;
}

bool
NodeBuilder::newNodeLoc(TokenPos *pos, MutableHandleValue dst)
{
    if (!pos) {
        dst.setNull();
        return true;
    }

    RootedObject loc(cx);
    RootedObject to(cx);
    RootedValue val(cx);

    if (!newObject(&loc))
        return false;

    dst.setObject(*loc);

    if (!newObject(&to))
        return false;
    val.setObject(*to);
    if (!setProperty(loc, "start", val))
        return false;
    val.setNumber(pos->begin.lineno);
    if (!setProperty(to, "line", val))
        return false;
    val.setNumber(pos->begin.index);
    if (!setProperty(to, "column", val))
        return false;

    if (!newObject(&to))
        return false;
    val.setObject(*to);
    if (!setProperty(loc, "end", val))
        return false;
    val.setNumber(pos->end.lineno);
    if (!setProperty(to, "line", val))
        return false;
    val.setNumber(pos->end.index);
    if (!setProperty(to, "column", val))
        return false;

    if (!setProperty(loc, "source", srcval))
        return false;

    return true;
}

bool
NodeBuilder::setNodeLoc(HandleObject node, TokenPos *pos)
{
    if (!saveLoc) {
        RootedValue nullVal(cx, NullValue());
        setProperty(node, "loc", nullVal);
        return true;
    }

    RootedValue loc(cx);
    return newNodeLoc(pos, &loc) &&
           setProperty(node, "loc", loc);
}

bool
NodeBuilder::program(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_PROGRAM, "body", elts, pos, dst);
}

bool
NodeBuilder::blockStatement(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_BLOCK_STMT, "body", elts, pos, dst);
}

bool
NodeBuilder::expressionStatement(HandleValue expr, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EXPR_STMT]);
    if (!cb.isNull())
        return callback(cb, expr, pos, dst);

    return newNode(AST_EXPR_STMT, pos, "expression", expr, dst);
}

bool
NodeBuilder::emptyStatement(TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_EMPTY_STMT]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_EMPTY_STMT, pos, dst);
}

bool
NodeBuilder::ifStatement(HandleValue test, HandleValue cons, HandleValue alt, TokenPos *pos,
                         MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_IF_STMT]);
    if (!cb.isNull())
        return callback(cb, test, cons, opt(alt), pos, dst);

    return newNode(AST_IF_STMT, pos,
                   "test", test,
                   "consequent", cons,
                   "alternate", alt,
                   dst);
}

bool
NodeBuilder::breakStatement(HandleValue label, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_BREAK_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(label), pos, dst);

    return newNode(AST_BREAK_STMT, pos, "label", label, dst);
}

bool
NodeBuilder::continueStatement(HandleValue label, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_CONTINUE_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(label), pos, dst);

    return newNode(AST_CONTINUE_STMT, pos, "label", label, dst);
}

bool
NodeBuilder::labeledStatement(HandleValue label, HandleValue stmt, TokenPos *pos,
                              MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_LAB_STMT]);
    if (!cb.isNull())
        return callback(cb, label, stmt, pos, dst);

    return newNode(AST_LAB_STMT, pos,
                   "label", label,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::throwStatement(HandleValue arg, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_THROW_STMT]);
    if (!cb.isNull())
        return callback(cb, arg, pos, dst);

    return newNode(AST_THROW_STMT, pos, "argument", arg, dst);
}

bool
NodeBuilder::returnStatement(HandleValue arg, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_RETURN_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(arg), pos, dst);

    return newNode(AST_RETURN_STMT, pos, "argument", arg, dst);
}

bool
NodeBuilder::forStatement(HandleValue init, HandleValue test, HandleValue update, HandleValue stmt,
                          TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_FOR_STMT]);
    if (!cb.isNull())
        return callback(cb, opt(init), opt(test), opt(update), stmt, pos, dst);

    return newNode(AST_FOR_STMT, pos,
                   "init", init,
                   "test", test,
                   "update", update,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::forInStatement(HandleValue var, HandleValue expr, HandleValue stmt, bool isForEach,
                            TokenPos *pos, MutableHandleValue dst)
{
    RootedValue isForEachVal(cx, BooleanValue(isForEach));

    RootedValue cb(cx, callbacks[AST_FOR_IN_STMT]);
    if (!cb.isNull())
        return callback(cb, var, expr, stmt, isForEachVal, pos, dst);

    return newNode(AST_FOR_IN_STMT, pos,
                   "left", var,
                   "right", expr,
                   "body", stmt,
                   "each", isForEachVal,
                   dst);
}

bool
NodeBuilder::forOfStatement(HandleValue var, HandleValue expr, HandleValue stmt, TokenPos *pos,
                            MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_FOR_OF_STMT]);
    if (!cb.isNull())
        return callback(cb, var, expr, stmt, pos, dst);

    return newNode(AST_FOR_OF_STMT, pos,
                   "left", var,
                   "right", expr,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::withStatement(HandleValue expr, HandleValue stmt, TokenPos *pos,
                           MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_WITH_STMT]);
    if (!cb.isNull())
        return callback(cb, expr, stmt, pos, dst);

    return newNode(AST_WITH_STMT, pos,
                   "object", expr,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::whileStatement(HandleValue test, HandleValue stmt, TokenPos *pos,
                            MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_WHILE_STMT]);
    if (!cb.isNull())
        return callback(cb, test, stmt, pos, dst);

    return newNode(AST_WHILE_STMT, pos,
                   "test", test,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::doWhileStatement(HandleValue stmt, HandleValue test, TokenPos *pos,
                              MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_DO_STMT]);
    if (!cb.isNull())
        return callback(cb, stmt, test, pos, dst);

    return newNode(AST_DO_STMT, pos,
                   "body", stmt,
                   "test", test,
                   dst);
}

bool
NodeBuilder::switchStatement(HandleValue disc, NodeVector &elts, bool lexical, TokenPos *pos,
                             MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(elts, &array))
        return false;

    RootedValue lexicalVal(cx, BooleanValue(lexical));

    RootedValue cb(cx, callbacks[AST_SWITCH_STMT]);
    if (!cb.isNull())
        return callback(cb, disc, array, lexicalVal, pos, dst);

    return newNode(AST_SWITCH_STMT, pos,
                   "discriminant", disc,
                   "cases", array,
                   "lexical", lexicalVal,
                   dst);
}

bool
NodeBuilder::tryStatement(HandleValue body, NodeVector &guarded, HandleValue unguarded,
                          HandleValue finally, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue guardedHandlers(cx);
    if (!newArray(guarded, &guardedHandlers))
        return false;

    RootedValue cb(cx, callbacks[AST_TRY_STMT]);
    if (!cb.isNull())
        return callback(cb, body, guardedHandlers, unguarded, opt(finally), pos, dst);

    return newNode(AST_TRY_STMT, pos,
                   "block", body,
                   "guardedHandlers", guardedHandlers,
                   "handler", unguarded,
                   "finalizer", finally,
                   dst);
}

bool
NodeBuilder::debuggerStatement(TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_DEBUGGER_STMT]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_DEBUGGER_STMT, pos, dst);
}

bool
NodeBuilder::binaryExpression(BinaryOperator op, HandleValue left, HandleValue right, TokenPos *pos,
                              MutableHandleValue dst)
{
    JS_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(binopNames[op], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_BINARY_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, left, right, pos, dst);

    return newNode(AST_BINARY_EXPR, pos,
                   "operator", opName,
                   "left", left,
                   "right", right,
                   dst);
}

bool
NodeBuilder::unaryExpression(UnaryOperator unop, HandleValue expr, TokenPos *pos,
                             MutableHandleValue dst)
{
    JS_ASSERT(unop > UNOP_ERR && unop < UNOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(unopNames[unop], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_UNARY_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, expr, pos, dst);

    RootedValue trueVal(cx, BooleanValue(true));
    return newNode(AST_UNARY_EXPR, pos,
                   "operator", opName,
                   "argument", expr,
                   "prefix", trueVal,
                   dst);
}

bool
NodeBuilder::assignmentExpression(AssignmentOperator aop, HandleValue lhs, HandleValue rhs,
                                  TokenPos *pos, MutableHandleValue dst)
{
    JS_ASSERT(aop > AOP_ERR && aop < AOP_LIMIT);

    RootedValue opName(cx);
    if (!atomValue(aopNames[aop], &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_ASSIGN_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, lhs, rhs, pos, dst);

    return newNode(AST_ASSIGN_EXPR, pos,
                   "operator", opName,
                   "left", lhs,
                   "right", rhs,
                   dst);
}

bool
NodeBuilder::updateExpression(HandleValue expr, bool incr, bool prefix, TokenPos *pos,
                              MutableHandleValue dst)
{
    RootedValue opName(cx);
    if (!atomValue(incr ? "++" : "--", &opName))
        return false;

    RootedValue prefixVal(cx, BooleanValue(prefix));

    RootedValue cb(cx, callbacks[AST_UPDATE_EXPR]);
    if (!cb.isNull())
        return callback(cb, expr, opName, prefixVal, pos, dst);

    return newNode(AST_UPDATE_EXPR, pos,
                   "operator", opName,
                   "argument", expr,
                   "prefix", prefixVal,
                   dst);
}

bool
NodeBuilder::logicalExpression(bool lor, HandleValue left, HandleValue right, TokenPos *pos,
                               MutableHandleValue dst)
{
    RootedValue opName(cx);
    if (!atomValue(lor ? "||" : "&&", &opName))
        return false;

    RootedValue cb(cx, callbacks[AST_LOGICAL_EXPR]);
    if (!cb.isNull())
        return callback(cb, opName, left, right, pos, dst);

    return newNode(AST_LOGICAL_EXPR, pos,
                   "operator", opName,
                   "left", left,
                   "right", right,
                   dst);
}

bool
NodeBuilder::conditionalExpression(HandleValue test, HandleValue cons, HandleValue alt,
                                   TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_COND_EXPR]);
    if (!cb.isNull())
        return callback(cb, test, cons, alt, pos, dst);

    return newNode(AST_COND_EXPR, pos,
                   "test", test,
                   "consequent", cons,
                   "alternate", alt,
                   dst);
}

bool
NodeBuilder::sequenceExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_LIST_EXPR, "expressions", elts, pos, dst);
}

bool
NodeBuilder::callExpression(HandleValue callee, NodeVector &args, TokenPos *pos,
                            MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(args, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_CALL_EXPR]);
    if (!cb.isNull())
        return callback(cb, callee, array, pos, dst);

    return newNode(AST_CALL_EXPR, pos,
                   "callee", callee,
                   "arguments", array,
                   dst);
}

bool
NodeBuilder::newExpression(HandleValue callee, NodeVector &args, TokenPos *pos,
                           MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(args, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_NEW_EXPR]);
    if (!cb.isNull())
        return callback(cb, callee, array, pos, dst);

    return newNode(AST_NEW_EXPR, pos,
                   "callee", callee,
                   "arguments", array,
                   dst);
}

bool
NodeBuilder::memberExpression(bool computed, HandleValue expr, HandleValue member, TokenPos *pos,
                              MutableHandleValue dst)
{
    RootedValue computedVal(cx, BooleanValue(computed));

    RootedValue cb(cx, callbacks[AST_MEMBER_EXPR]);
    if (!cb.isNull())
        return callback(cb, computedVal, expr, member, pos, dst);

    return newNode(AST_MEMBER_EXPR, pos,
                   "object", expr,
                   "property", member,
                   "computed", computedVal,
                   dst);
}

bool
NodeBuilder::arrayExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_ARRAY_EXPR, "elements", elts, pos, dst);
}

bool
NodeBuilder::spreadExpression(HandleValue expr, TokenPos *pos, MutableHandleValue dst)
{
    return newNode(AST_SPREAD_EXPR, pos,
                   "expression", expr,
                   dst);
}

bool
NodeBuilder::propertyPattern(HandleValue key, HandleValue patt, TokenPos *pos,
                             MutableHandleValue dst)
{
    RootedValue kindName(cx);
    if (!atomValue("init", &kindName))
        return false;

    RootedValue cb(cx, callbacks[AST_PROP_PATT]);
    if (!cb.isNull())
        return callback(cb, key, patt, pos, dst);

    return newNode(AST_PROP_PATT, pos,
                   "key", key,
                   "value", patt,
                   "kind", kindName,
                   dst);
}

bool
NodeBuilder::propertyInitializer(HandleValue key, HandleValue val, PropKind kind, TokenPos *pos,
                                 MutableHandleValue dst)
{
    RootedValue kindName(cx);
    if (!atomValue(kind == PROP_INIT
                   ? "init"
                   : kind == PROP_GETTER
                   ? "get"
                   : "set", &kindName)) {
        return false;
    }

    RootedValue cb(cx, callbacks[AST_PROPERTY]);
    if (!cb.isNull())
        return callback(cb, kindName, key, val, pos, dst);

    return newNode(AST_PROPERTY, pos,
                   "key", key,
                   "value", val,
                   "kind", kindName,
                   dst);
}

bool
NodeBuilder::objectExpression(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_OBJECT_EXPR, "properties", elts, pos, dst);
}

bool
NodeBuilder::thisExpression(TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_THIS_EXPR]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_THIS_EXPR, pos, dst);
}

bool
NodeBuilder::yieldExpression(HandleValue arg, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_YIELD_EXPR]);
    if (!cb.isNull())
        return callback(cb, opt(arg), pos, dst);

    return newNode(AST_YIELD_EXPR, pos, "argument", arg, dst);
}

bool
NodeBuilder::comprehensionBlock(HandleValue patt, HandleValue src, bool isForEach, TokenPos *pos,
                                MutableHandleValue dst)
{
    RootedValue isForEachVal(cx, BooleanValue(isForEach));

    RootedValue cb(cx, callbacks[AST_COMP_BLOCK]);
    if (!cb.isNull())
        return callback(cb, patt, src, isForEachVal, pos, dst);

    return newNode(AST_COMP_BLOCK, pos,
                   "left", patt,
                   "right", src,
                   "each", isForEachVal,
                   dst);
}

bool
NodeBuilder::comprehensionExpression(HandleValue body, NodeVector &blocks, HandleValue filter,
                                     TokenPos *pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(blocks, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_COMP_EXPR]);
    if (!cb.isNull())
        return callback(cb, body, array, opt(filter), pos, dst);

    return newNode(AST_COMP_EXPR, pos,
                   "body", body,
                   "blocks", array,
                   "filter", filter,
                   dst);
}

bool
NodeBuilder::generatorExpression(HandleValue body, NodeVector &blocks, HandleValue filter,
                                 TokenPos *pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(blocks, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_GENERATOR_EXPR]);
    if (!cb.isNull())
        return callback(cb, body, array, opt(filter), pos, dst);

    return newNode(AST_GENERATOR_EXPR, pos,
                   "body", body,
                   "blocks", array,
                   "filter", filter,
                   dst);
}

bool
NodeBuilder::letExpression(NodeVector &head, HandleValue expr, TokenPos *pos,
                           MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(head, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_LET_EXPR]);
    if (!cb.isNull())
        return callback(cb, array, expr, pos, dst);

    return newNode(AST_LET_EXPR, pos,
                   "head", array,
                   "body", expr,
                   dst);
}

bool
NodeBuilder::letStatement(NodeVector &head, HandleValue stmt, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(head, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_LET_STMT]);
    if (!cb.isNull())
        return callback(cb, array, stmt, pos, dst);

    return newNode(AST_LET_STMT, pos,
                   "head", array,
                   "body", stmt,
                   dst);
}

bool
NodeBuilder::variableDeclaration(NodeVector &elts, VarDeclKind kind, TokenPos *pos,
                                 MutableHandleValue dst)
{
    JS_ASSERT(kind > VARDECL_ERR && kind < VARDECL_LIMIT);

    RootedValue array(cx), kindName(cx);
    if (!newArray(elts, &array) ||
        !atomValue(kind == VARDECL_CONST
                   ? "const"
                   : kind == VARDECL_LET
                   ? "let"
                   : "var", &kindName)) {
        return false;
    }

    RootedValue cb(cx, callbacks[AST_VAR_DECL]);
    if (!cb.isNull())
        return callback(cb, kindName, array, pos, dst);

    return newNode(AST_VAR_DECL, pos,
                   "kind", kindName,
                   "declarations", array,
                   dst);
}

bool
NodeBuilder::variableDeclarator(HandleValue id, HandleValue init, TokenPos *pos,
                                MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_VAR_DTOR]);
    if (!cb.isNull())
        return callback(cb, id, opt(init), pos, dst);

    return newNode(AST_VAR_DTOR, pos, "id", id, "init", init, dst);
}

bool
NodeBuilder::switchCase(HandleValue expr, NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue array(cx);
    if (!newArray(elts, &array))
        return false;

    RootedValue cb(cx, callbacks[AST_CASE]);
    if (!cb.isNull())
        return callback(cb, opt(expr), array, pos, dst);

    return newNode(AST_CASE, pos,
                   "test", expr,
                   "consequent", array,
                   dst);
}

bool
NodeBuilder::catchClause(HandleValue var, HandleValue guard, HandleValue body, TokenPos *pos,
                         MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_CATCH]);
    if (!cb.isNull())
        return callback(cb, var, opt(guard), body, pos, dst);

    return newNode(AST_CATCH, pos,
                   "param", var,
                   "guard", guard,
                   "body", body,
                   dst);
}

bool
NodeBuilder::literal(HandleValue val, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_LITERAL]);
    if (!cb.isNull())
        return callback(cb, val, pos, dst);

    return newNode(AST_LITERAL, pos, "value", val, dst);
}

bool
NodeBuilder::identifier(HandleValue name, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_IDENTIFIER]);
    if (!cb.isNull())
        return callback(cb, name, pos, dst);

    return newNode(AST_IDENTIFIER, pos, "name", name, dst);
}

bool
NodeBuilder::objectPattern(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_OBJECT_PATT, "properties", elts, pos, dst);
}

bool
NodeBuilder::arrayPattern(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_ARRAY_PATT, "elements", elts, pos, dst);
}

bool
NodeBuilder::function(ASTType type, TokenPos *pos,
                      HandleValue id, NodeVector &args, NodeVector &defaults,
                      HandleValue body, HandleValue rest,
                      bool isGenerator, bool isExpression,
                      MutableHandleValue dst)
{
    RootedValue array(cx), defarray(cx);
    if (!newArray(args, &array))
        return false;
    if (!newArray(defaults, &defarray))
        return false;

    RootedValue isGeneratorVal(cx, BooleanValue(isGenerator));
    RootedValue isExpressionVal(cx, BooleanValue(isExpression));

    RootedValue cb(cx, callbacks[type]);
    if (!cb.isNull()) {
        return callback(cb, opt(id), array, body, isGeneratorVal, isExpressionVal, pos, dst);
    }

    return newNode(type, pos,
                   "id", id,
                   "params", array,
                   "defaults", defarray,
                   "body", body,
                   "rest", rest,
                   "generator", isGeneratorVal,
                   "expression", isExpressionVal,
                   dst);
}

bool
NodeBuilder::xmlAnyName(TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLANYNAME]);
    if (!cb.isNull())
        return callback(cb, pos, dst);

    return newNode(AST_XMLANYNAME, pos, dst);
}

bool
NodeBuilder::xmlEscapeExpression(HandleValue expr, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLESCAPE]);
    if (!cb.isNull())
        return callback(cb, expr, pos, dst);

    return newNode(AST_XMLESCAPE, pos, "expression", expr, dst);
}

bool
NodeBuilder::xmlFilterExpression(HandleValue left, HandleValue right, TokenPos *pos,
                                 MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLFILTER]);
    if (!cb.isNull())
        return callback(cb, left, right, pos, dst);

    return newNode(AST_XMLFILTER, pos, "left", left, "right", right, dst);
}

bool
NodeBuilder::xmlDefaultNamespace(HandleValue ns, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLDEFAULT]);
    if (!cb.isNull())
        return callback(cb, ns, pos, dst);

    return newNode(AST_XMLDEFAULT, pos, "namespace", ns, dst);
}

bool
NodeBuilder::xmlAttributeSelector(HandleValue expr, bool computed, TokenPos *pos,
                                  MutableHandleValue dst)
{
    RootedValue computedVal(cx, BooleanValue(computed));

    RootedValue cb(cx, callbacks[AST_XMLATTR_SEL]);
    if (!cb.isNull())
        return callback(cb, expr, computedVal, pos, dst);

    return newNode(AST_XMLATTR_SEL, pos,
                   "attribute", expr,
                   "computed", computedVal,
                   dst);
}

bool
NodeBuilder::xmlFunctionQualifiedIdentifier(HandleValue right, bool computed, TokenPos *pos,
                                            MutableHandleValue dst)
{
    RootedValue computedVal(cx, BooleanValue(computed));

    RootedValue cb(cx, callbacks[AST_XMLFUNCQUAL]);
    if (!cb.isNull())
        return callback(cb, right, computedVal, pos, dst);

    return newNode(AST_XMLFUNCQUAL, pos,
                   "right", right,
                   "computed", computedVal,
                   dst);
}

bool
NodeBuilder::xmlQualifiedIdentifier(HandleValue left, HandleValue right, bool computed,
                                    TokenPos *pos, MutableHandleValue dst)
{
    RootedValue computedVal(cx, BooleanValue(computed));

    RootedValue cb(cx, callbacks[AST_XMLQUAL]);
    if (!cb.isNull())
        return callback(cb, left, right, computedVal, pos, dst);

    return newNode(AST_XMLQUAL, pos,
                   "left", left,
                   "right", right,
                   "computed", computedVal,
                   dst);
}

bool
NodeBuilder::xmlElement(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLELEM, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlText(HandleValue text, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLTEXT]);
    if (!cb.isNull())
        return callback(cb, text, pos, dst);

    return newNode(AST_XMLTEXT, pos, "text", text, dst);
}

bool
NodeBuilder::xmlList(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLLIST, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlStartTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLSTART, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlEndTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLEND, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlPointTag(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLPOINT, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlName(HandleValue text, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLNAME]);
    if (!cb.isNull())
        return callback(cb, text, pos, dst);

    return newNode(AST_XMLNAME, pos, "contents", text, dst);
}

bool
NodeBuilder::xmlName(NodeVector &elts, TokenPos *pos, MutableHandleValue dst)
{
    return listNode(AST_XMLNAME, "contents", elts, pos, dst);
}

bool
NodeBuilder::xmlAttribute(HandleValue text, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLATTR]);
    if (!cb.isNull())
        return callback(cb, text, pos, dst);

    return newNode(AST_XMLATTR, pos, "value", text, dst);
}

bool
NodeBuilder::xmlCdata(HandleValue text, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLCDATA]);
    if (!cb.isNull())
        return callback(cb, text, pos, dst);

    return newNode(AST_XMLCDATA, pos, "contents", text, dst);
}

bool
NodeBuilder::xmlComment(HandleValue text, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLCOMMENT]);
    if (!cb.isNull())
        return callback(cb, text, pos, dst);

    return newNode(AST_XMLCOMMENT, pos, "contents", text, dst);
}

bool
NodeBuilder::xmlPI(HandleValue target, HandleValue contents, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue cb(cx, callbacks[AST_XMLPI]);
    if (!cb.isNull())
        return callback(cb, target, contents, pos, dst);

    return newNode(AST_XMLPI, pos,
                   "target", target,
                   "contents", contents,
                   dst);
}


/*
 * Serialization of parse nodes to JavaScript objects.
 *
 * All serialization methods take a non-nullable ParseNode pointer.
 */

class ASTSerializer
{
    JSContext           *cx;
    Parser              *parser;
    NodeBuilder         builder;
    DebugOnly<uint32_t> lineno;

    RawValue unrootedAtomContents(RawAtom atom) {
        return StringValue(atom ? atom : cx->names().empty);
    }

    BinaryOperator binop(ParseNodeKind kind, JSOp op);
    UnaryOperator unop(ParseNodeKind kind, JSOp op);
    AssignmentOperator aop(JSOp op);

    bool statements(ParseNode *pn, NodeVector &elts);
    bool expressions(ParseNode *pn, NodeVector &elts);
    bool xmls(ParseNode *pn, NodeVector &elts);
    bool leftAssociate(ParseNode *pn, MutableHandleValue dst);
    bool functionArgs(ParseNode *pn, ParseNode *pnargs, ParseNode *pndestruct, ParseNode *pnbody,
                      NodeVector &args, NodeVector &defaults, MutableHandleValue rest);

    bool sourceElement(ParseNode *pn, MutableHandleValue dst);

    bool declaration(ParseNode *pn, MutableHandleValue dst);
    bool variableDeclaration(ParseNode *pn, bool let, MutableHandleValue dst);
    bool variableDeclarator(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst);
    bool let(ParseNode *pn, bool expr, MutableHandleValue dst);

    bool optStatement(ParseNode *pn, MutableHandleValue dst) {
        if (!pn) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return statement(pn, dst);
    }

    bool forInit(ParseNode *pn, MutableHandleValue dst);
    bool forOfOrIn(ParseNode *loop, ParseNode *head, HandleValue var, HandleValue stmt,
                   MutableHandleValue dst);
    bool statement(ParseNode *pn, MutableHandleValue dst);
    bool blockStatement(ParseNode *pn, MutableHandleValue dst);
    bool switchStatement(ParseNode *pn, MutableHandleValue dst);
    bool switchCase(ParseNode *pn, MutableHandleValue dst);
    bool tryStatement(ParseNode *pn, MutableHandleValue dst);
    bool catchClause(ParseNode *pn, bool *isGuarded, MutableHandleValue dst);

    bool optExpression(ParseNode *pn, MutableHandleValue dst) {
        if (!pn) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return expression(pn, dst);
    }

    bool expression(ParseNode *pn, MutableHandleValue dst);

    bool propertyName(ParseNode *pn, MutableHandleValue dst);
    bool property(ParseNode *pn, MutableHandleValue dst);

    bool optIdentifier(HandleAtom atom, TokenPos *pos, MutableHandleValue dst) {
        if (!atom) {
            dst.setMagic(JS_SERIALIZE_NO_NODE);
            return true;
        }
        return identifier(atom, pos, dst);
    }

    bool identifier(HandleAtom atom, TokenPos *pos, MutableHandleValue dst);
    bool identifier(ParseNode *pn, MutableHandleValue dst);
    bool literal(ParseNode *pn, MutableHandleValue dst);

    bool pattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst);
    bool arrayPattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst);
    bool objectPattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst);

    bool function(ParseNode *pn, ASTType type, MutableHandleValue dst);
    bool functionArgsAndBody(ParseNode *pn, NodeVector &args, NodeVector &defaults,
                             MutableHandleValue body, MutableHandleValue rest);
    bool functionBody(ParseNode *pn, TokenPos *pos, MutableHandleValue dst);

    bool comprehensionBlock(ParseNode *pn, MutableHandleValue dst);
    bool comprehension(ParseNode *pn, MutableHandleValue dst);
    bool generatorExpression(ParseNode *pn, MutableHandleValue dst);

    bool xml(ParseNode *pn, MutableHandleValue dst);

  public:
    ASTSerializer(JSContext *c, bool l, char const *src, uint32_t ln)
        : cx(c)
        , builder(c, l, src)
#ifdef DEBUG
        , lineno(ln)
#endif
    {}

    bool init(HandleObject userobj) {
        return builder.init(userobj);
    }

    void setParser(Parser *p) {
        parser = p;
    }

    bool program(ParseNode *pn, MutableHandleValue dst);
};

AssignmentOperator
ASTSerializer::aop(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return AOP_ASSIGN;
      case JSOP_ADD:
        return AOP_PLUS;
      case JSOP_SUB:
        return AOP_MINUS;
      case JSOP_MUL:
        return AOP_STAR;
      case JSOP_DIV:
        return AOP_DIV;
      case JSOP_MOD:
        return AOP_MOD;
      case JSOP_LSH:
        return AOP_LSH;
      case JSOP_RSH:
        return AOP_RSH;
      case JSOP_URSH:
        return AOP_URSH;
      case JSOP_BITOR:
        return AOP_BITOR;
      case JSOP_BITXOR:
        return AOP_BITXOR;
      case JSOP_BITAND:
        return AOP_BITAND;
      default:
        return AOP_ERR;
    }
}

UnaryOperator
ASTSerializer::unop(ParseNodeKind kind, JSOp op)
{
    if (kind == PNK_DELETE)
        return UNOP_DELETE;

    switch (op) {
      case JSOP_NEG:
        return UNOP_NEG;
      case JSOP_POS:
        return UNOP_POS;
      case JSOP_NOT:
        return UNOP_NOT;
      case JSOP_BITNOT:
        return UNOP_BITNOT;
      case JSOP_TYPEOF:
      case JSOP_TYPEOFEXPR:
        return UNOP_TYPEOF;
      case JSOP_VOID:
        return UNOP_VOID;
      default:
        return UNOP_ERR;
    }
}

BinaryOperator
ASTSerializer::binop(ParseNodeKind kind, JSOp op)
{
    switch (kind) {
      case PNK_LSH:
        return BINOP_LSH;
      case PNK_RSH:
        return BINOP_RSH;
      case PNK_URSH:
        return BINOP_URSH;
      case PNK_LT:
        return BINOP_LT;
      case PNK_LE:
        return BINOP_LE;
      case PNK_GT:
        return BINOP_GT;
      case PNK_GE:
        return BINOP_GE;
      case PNK_EQ:
        return BINOP_EQ;
      case PNK_NE:
        return BINOP_NE;
      case PNK_STRICTEQ:
        return BINOP_STRICTEQ;
      case PNK_STRICTNE:
        return BINOP_STRICTNE;
      case PNK_ADD:
        return BINOP_ADD;
      case PNK_SUB:
        return BINOP_SUB;
      case PNK_STAR:
        return BINOP_STAR;
      case PNK_DIV:
        return BINOP_DIV;
      case PNK_MOD:
        return BINOP_MOD;
      case PNK_BITOR:
        return BINOP_BITOR;
      case PNK_BITXOR:
        return BINOP_BITXOR;
      case PNK_BITAND:
        return BINOP_BITAND;
      case PNK_IN:
        return BINOP_IN;
      case PNK_INSTANCEOF:
        return BINOP_INSTANCEOF;
      case PNK_DBLDOT:
        return BINOP_DBLDOT;
      default:
        return BINOP_ERR;
    }
}

bool
ASTSerializer::statements(ParseNode *pn, NodeVector &elts)
{
    JS_ASSERT(pn->isKind(PNK_STATEMENTLIST));
    JS_ASSERT(pn->isArity(PN_LIST));

    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue elt(cx);
        if (!sourceElement(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    return true;
}

bool
ASTSerializer::expressions(ParseNode *pn, NodeVector &elts)
{
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue elt(cx);
        if (!expression(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    return true;
}

bool
ASTSerializer::xmls(ParseNode *pn, NodeVector &elts)
{
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue elt(cx);
        if (!xml(next, &elt))
            return false;
        elts.infallibleAppend(elt);
    }

    return true;
}

bool
ASTSerializer::blockStatement(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->isKind(PNK_STATEMENTLIST));

    NodeVector stmts(cx);
    return statements(pn, stmts) &&
           builder.blockStatement(stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::program(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->pn_pos.begin.lineno == lineno);

    NodeVector stmts(cx);
    return statements(pn, stmts) &&
           builder.program(stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::sourceElement(ParseNode *pn, MutableHandleValue dst)
{
    /* SpiderMonkey allows declarations even in pure statement contexts. */
    return statement(pn, dst);
}

bool
ASTSerializer::declaration(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->isKind(PNK_FUNCTION) ||
              pn->isKind(PNK_VAR) ||
              pn->isKind(PNK_LET) ||
              pn->isKind(PNK_CONST));

    switch (pn->getKind()) {
      case PNK_FUNCTION:
        return function(pn, AST_FUNC_DECL, dst);

      case PNK_VAR:
      case PNK_CONST:
        return variableDeclaration(pn, false, dst);

      default:
        JS_ASSERT(pn->isKind(PNK_LET));
        return variableDeclaration(pn, true, dst);
    }
}

bool
ASTSerializer::variableDeclaration(ParseNode *pn, bool let, MutableHandleValue dst)
{
    JS_ASSERT(let ? pn->isKind(PNK_LET) : (pn->isKind(PNK_VAR) || pn->isKind(PNK_CONST)));

    /* Later updated to VARDECL_CONST if we find a PND_CONST declarator. */
    VarDeclKind kind = let ? VARDECL_LET : VARDECL_VAR;

    NodeVector dtors(cx);

    /* In a for-in context, variable declarations contain just a single pattern. */
    if (pn->pn_xflags & PNX_FORINVAR) {
        RootedValue patt(cx), child(cx);
        RootedValue nullVal(cx, NullValue());
        return pattern(pn->pn_head, &kind, &patt) &&
               builder.variableDeclarator(patt, nullVal, &pn->pn_head->pn_pos, &child) &&
               dtors.append(child) &&
               builder.variableDeclaration(dtors, kind, &pn->pn_pos, dst);
    }

    if (!dtors.reserve(pn->pn_count))
        return false;
    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!variableDeclarator(next, &kind, &child))
            return false;
        dtors.infallibleAppend(child);
    }

    return builder.variableDeclaration(dtors, kind, &pn->pn_pos, dst);
}

bool
ASTSerializer::variableDeclarator(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst)
{
    /* A destructuring declarator is always a PNK_ASSIGN. */
    JS_ASSERT(pn->isKind(PNK_NAME) || pn->isKind(PNK_ASSIGN));

    ParseNode *pnleft;
    ParseNode *pnright;

    if (pn->isKind(PNK_NAME)) {
        pnleft = pn;
        pnright = pn->isUsed() ? NULL : pn->pn_expr;
        JS_ASSERT_IF(pnright, pn->pn_pos.encloses(pnright->pn_pos));
    } else {
        JS_ASSERT(pn->isKind(PNK_ASSIGN));
        pnleft = pn->pn_left;
        pnright = pn->pn_right;
        JS_ASSERT(pn->pn_pos.encloses(pnleft->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pnright->pn_pos));
    }

    RootedValue left(cx), right(cx);
    return pattern(pnleft, pkind, &left) &&
           optExpression(pnright, &right) &&
           builder.variableDeclarator(left, right, &pn->pn_pos, dst);
}

bool
ASTSerializer::let(ParseNode *pn, bool expr, MutableHandleValue dst)
{
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    ParseNode *letHead = pn->pn_left;
    LOCAL_ASSERT(letHead->isArity(PN_LIST));

    ParseNode *letBody = pn->pn_right;
    LOCAL_ASSERT(letBody->isKind(PNK_LEXICALSCOPE));

    NodeVector dtors(cx);
    if (!dtors.reserve(letHead->pn_count))
        return false;

    VarDeclKind kind = VARDECL_LET_HEAD;

    for (ParseNode *next = letHead->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);
        /*
         * Unlike in |variableDeclaration|, this does not update |kind|; since let-heads do
         * not contain const declarations, declarators should never have PND_CONST set.
         */
        if (!variableDeclarator(next, &kind, &child))
            return false;
        dtors.infallibleAppend(child);
    }

    RootedValue v(cx);
    return expr
           ? expression(letBody->pn_expr, &v) &&
             builder.letExpression(dtors, v, &pn->pn_pos, dst)
           : statement(letBody->pn_expr, &v) &&
             builder.letStatement(dtors, v, &pn->pn_pos, dst);
}

bool
ASTSerializer::switchCase(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT_IF(pn->pn_left, pn->pn_pos.encloses(pn->pn_left->pn_pos));
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    NodeVector stmts(cx);

    RootedValue expr(cx);

    return optExpression(pn->pn_left, &expr) &&
           statements(pn->pn_right, stmts) &&
           builder.switchCase(expr, stmts, &pn->pn_pos, dst);
}

bool
ASTSerializer::switchStatement(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

    RootedValue disc(cx);

    if (!expression(pn->pn_left, &disc))
        return false;

    ParseNode *listNode;
    bool lexical;

    if (pn->pn_right->isKind(PNK_LEXICALSCOPE)) {
        listNode = pn->pn_right->pn_expr;
        lexical = true;
    } else {
        listNode = pn->pn_right;
        lexical = false;
    }

    NodeVector cases(cx);
    if (!cases.reserve(listNode->pn_count))
        return false;

    for (ParseNode *next = listNode->pn_head; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!switchCase(next, &child))
            return false;
        cases.infallibleAppend(child);
    }

    return builder.switchStatement(disc, cases, lexical, &pn->pn_pos, dst);
}

bool
ASTSerializer::catchClause(ParseNode *pn, bool *isGuarded, MutableHandleValue dst)
{
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
    JS_ASSERT_IF(pn->pn_kid2, pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

    RootedValue var(cx), guard(cx), body(cx);

    if (!pattern(pn->pn_kid1, NULL, &var) ||
        !optExpression(pn->pn_kid2, &guard)) {
        return false;
    }

    *isGuarded = !guard.isMagic(JS_SERIALIZE_NO_NODE);

    return statement(pn->pn_kid3, &body) &&
           builder.catchClause(var, guard, body, &pn->pn_pos, dst);
}

bool
ASTSerializer::tryStatement(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
    JS_ASSERT_IF(pn->pn_kid2, pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
    JS_ASSERT_IF(pn->pn_kid3, pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

    RootedValue body(cx);
    if (!statement(pn->pn_kid1, &body))
        return false;

    NodeVector guarded(cx);
    RootedValue unguarded(cx, NullValue());

    if (pn->pn_kid2) {
        if (!guarded.reserve(pn->pn_kid2->pn_count))
            return false;

        for (ParseNode *next = pn->pn_kid2->pn_head; next; next = next->pn_next) {
            RootedValue clause(cx);
            bool isGuarded;
            if (!catchClause(next->pn_expr, &isGuarded, &clause))
                return false;
            if (isGuarded)
                guarded.infallibleAppend(clause);
            else
                unguarded = clause;
        }
    }

    RootedValue finally(cx);
    return optStatement(pn->pn_kid3, &finally) &&
           builder.tryStatement(body, guarded, unguarded, finally, &pn->pn_pos, dst);
}

bool
ASTSerializer::forInit(ParseNode *pn, MutableHandleValue dst)
{
    if (!pn) {
        dst.setMagic(JS_SERIALIZE_NO_NODE);
        return true;
    }

    return (pn->isKind(PNK_VAR) || pn->isKind(PNK_CONST))
           ? variableDeclaration(pn, false, dst)
           : expression(pn, dst);
}

bool
ASTSerializer::forOfOrIn(ParseNode *loop, ParseNode *head, HandleValue var, HandleValue stmt,
                         MutableHandleValue dst)
{
    RootedValue expr(cx);
    bool isForEach = loop->pn_iflags & JSITER_FOREACH;
    bool isForOf = loop->pn_iflags & JSITER_FOR_OF;
    JS_ASSERT(!isForOf || !isForEach);

    return expression(head->pn_kid3, &expr) &&
        (isForOf ? builder.forOfStatement(var, expr, stmt, &loop->pn_pos, dst) :
         builder.forInStatement(var, expr, stmt, isForEach, &loop->pn_pos, dst));
}

bool
ASTSerializer::statement(ParseNode *pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_FUNCTION:
      case PNK_VAR:
      case PNK_CONST:
        return declaration(pn, dst);

      case PNK_LET:
        return pn->isArity(PN_BINARY)
               ? let(pn, false, dst)
               : declaration(pn, dst);

      case PNK_NAME:
        LOCAL_ASSERT(pn->isUsed());
        return statement(pn->pn_lexdef, dst);

      case PNK_SEMI:
        if (pn->pn_kid) {
            RootedValue expr(cx);
            return expression(pn->pn_kid, &expr) &&
                   builder.expressionStatement(expr, &pn->pn_pos, dst);
        }
        return builder.emptyStatement(&pn->pn_pos, dst);

      case PNK_LEXICALSCOPE:
        pn = pn->pn_expr;
        if (!pn->isKind(PNK_STATEMENTLIST))
            return statement(pn, dst);
        /* FALL THROUGH */

      case PNK_STATEMENTLIST:
        return blockStatement(pn, dst);

      case PNK_IF:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
        JS_ASSERT_IF(pn->pn_kid3, pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

        RootedValue test(cx), cons(cx), alt(cx);

        return expression(pn->pn_kid1, &test) &&
               statement(pn->pn_kid2, &cons) &&
               optStatement(pn->pn_kid3, &alt) &&
               builder.ifStatement(test, cons, alt, &pn->pn_pos, dst);
      }

      case PNK_SWITCH:
        return switchStatement(pn, dst);

      case PNK_TRY:
        return tryStatement(pn, dst);

      case PNK_WITH:
      case PNK_WHILE:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue expr(cx), stmt(cx);

        return expression(pn->pn_left, &expr) &&
               statement(pn->pn_right, &stmt) &&
               (pn->isKind(PNK_WITH)
                ? builder.withStatement(expr, stmt, &pn->pn_pos, dst)
                : builder.whileStatement(expr, stmt, &pn->pn_pos, dst));
      }

      case PNK_DOWHILE:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue stmt(cx), test(cx);

        return statement(pn->pn_left, &stmt) &&
               expression(pn->pn_right, &test) &&
               builder.doWhileStatement(stmt, test, &pn->pn_pos, dst);
      }

      case PNK_FOR:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        ParseNode *head = pn->pn_left;

        JS_ASSERT_IF(head->pn_kid1, head->pn_pos.encloses(head->pn_kid1->pn_pos));
        JS_ASSERT_IF(head->pn_kid2, head->pn_pos.encloses(head->pn_kid2->pn_pos));
        JS_ASSERT_IF(head->pn_kid3, head->pn_pos.encloses(head->pn_kid3->pn_pos));

        RootedValue stmt(cx);
        if (!statement(pn->pn_right, &stmt))
            return false;

        if (head->isKind(PNK_FORIN)) {
            RootedValue var(cx);
            return (!head->pn_kid1
                    ? pattern(head->pn_kid2, NULL, &var)
                    : head->pn_kid1->isKind(PNK_LEXICALSCOPE)
                    ? variableDeclaration(head->pn_kid1->pn_expr, true, &var)
                    : variableDeclaration(head->pn_kid1, false, &var)) &&
                forOfOrIn(pn, head, var, stmt, dst);
        }

        RootedValue init(cx), test(cx), update(cx);

        return forInit(head->pn_kid1, &init) &&
               optExpression(head->pn_kid2, &test) &&
               optExpression(head->pn_kid3, &update) &&
               builder.forStatement(init, test, update, stmt, &pn->pn_pos, dst);
      }

      /* Synthesized by the parser when a for-in loop contains a variable initializer. */
      case PNK_SEQ:
      {
        LOCAL_ASSERT(pn->pn_count == 2);

        ParseNode *prelude = pn->pn_head;
        ParseNode *loop = prelude->pn_next;

        LOCAL_ASSERT(prelude->isKind(PNK_VAR) && loop->isKind(PNK_FOR));

        RootedValue var(cx);
        if (!variableDeclaration(prelude, false, &var))
            return false;

        ParseNode *head = loop->pn_left;
        JS_ASSERT(head->isKind(PNK_FORIN));

        RootedValue stmt(cx);

        return statement(loop->pn_right, &stmt) && forOfOrIn(loop, head, var, stmt, dst);
      }

      case PNK_BREAK:
      case PNK_CONTINUE:
      {
        RootedValue label(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);
        return optIdentifier(pnAtom, NULL, &label) &&
               (pn->isKind(PNK_BREAK)
                ? builder.breakStatement(label, &pn->pn_pos, dst)
                : builder.continueStatement(label, &pn->pn_pos, dst));
      }

      case PNK_COLON:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

        RootedValue label(cx), stmt(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);
        return identifier(pnAtom, NULL, &label) &&
               statement(pn->pn_expr, &stmt) &&
               builder.labeledStatement(label, stmt, &pn->pn_pos, dst);
      }

      case PNK_THROW:
      case PNK_RETURN:
      {
        JS_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);

        return optExpression(pn->pn_kid, &arg) &&
               (pn->isKind(PNK_THROW)
                ? builder.throwStatement(arg, &pn->pn_pos, dst)
                : builder.returnStatement(arg, &pn->pn_pos, dst));
      }

      case PNK_DEBUGGER:
        return builder.debuggerStatement(&pn->pn_pos, dst);

#if JS_HAS_XML_SUPPORT
      case PNK_DEFXMLNS:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        LOCAL_ASSERT(pn->isArity(PN_UNARY));

        RootedValue ns(cx);

        return expression(pn->pn_kid, &ns) &&
               builder.xmlDefaultNamespace(ns, &pn->pn_pos, dst);
      }
#endif

      case PNK_NOP:
        return builder.emptyStatement(&pn->pn_pos, dst);

      default:
        LOCAL_NOT_REACHED("unexpected statement type");
    }
}

bool
ASTSerializer::leftAssociate(ParseNode *pn, MutableHandleValue dst)
{
    JS_ASSERT(pn->isArity(PN_LIST));
    JS_ASSERT(pn->pn_count >= 1);

    ParseNodeKind kind = pn->getKind();
    bool lor = kind == PNK_OR;
    bool logop = lor || (kind == PNK_AND);

    ParseNode *head = pn->pn_head;
    RootedValue left(cx);
    if (!expression(head, &left))
        return false;
    for (ParseNode *next = head->pn_next; next; next = next->pn_next) {
        RootedValue right(cx);
        if (!expression(next, &right))
            return false;

        TokenPos subpos = {pn->pn_pos.begin, next->pn_pos.end};

        if (logop) {
            if (!builder.logicalExpression(lor, left, right, &subpos, &left))
                return false;
        } else {
            BinaryOperator op = binop(pn->getKind(), pn->getOp());
            LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

            if (!builder.binaryExpression(op, left, right, &subpos, &left))
                return false;
        }
    }

    dst.set(left);
    return true;
}

bool
ASTSerializer::comprehensionBlock(ParseNode *pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isArity(PN_BINARY));

    ParseNode *in = pn->pn_left;

    LOCAL_ASSERT(in && in->isKind(PNK_FORIN));

    bool isForEach = pn->pn_iflags & JSITER_FOREACH;

    RootedValue patt(cx), src(cx);
    return pattern(in->pn_kid2, NULL, &patt) &&
           expression(in->pn_kid3, &src) &&
           builder.comprehensionBlock(patt, src, isForEach, &in->pn_pos, dst);
}

bool
ASTSerializer::comprehension(ParseNode *pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isKind(PNK_FOR));

    NodeVector blocks(cx);

    ParseNode *next = pn;
    while (next->isKind(PNK_FOR)) {
        RootedValue block(cx);
        if (!comprehensionBlock(next, &block) || !blocks.append(block))
            return false;
        next = next->pn_right;
    }

    RootedValue filter(cx, MagicValue(JS_SERIALIZE_NO_NODE));

    if (next->isKind(PNK_IF)) {
        if (!optExpression(next->pn_kid1, &filter))
            return false;
        next = next->pn_kid2;
    } else if (next->isKind(PNK_STATEMENTLIST) && next->pn_count == 0) {
        /* FoldConstants optimized away the push. */
        NodeVector empty(cx);
        return builder.arrayExpression(empty, &pn->pn_pos, dst);
    }

    LOCAL_ASSERT(next->isKind(PNK_ARRAYPUSH));

    RootedValue body(cx);

    return expression(next->pn_kid, &body) &&
           builder.comprehensionExpression(body, blocks, filter, &pn->pn_pos, dst);
}

bool
ASTSerializer::generatorExpression(ParseNode *pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isKind(PNK_FOR));

    NodeVector blocks(cx);

    ParseNode *next = pn;
    while (next->isKind(PNK_FOR)) {
        RootedValue block(cx);
        if (!comprehensionBlock(next, &block) || !blocks.append(block))
            return false;
        next = next->pn_right;
    }

    RootedValue filter(cx, MagicValue(JS_SERIALIZE_NO_NODE));

    if (next->isKind(PNK_IF)) {
        if (!optExpression(next->pn_kid1, &filter))
            return false;
        next = next->pn_kid2;
    }

    LOCAL_ASSERT(next->isKind(PNK_SEMI) &&
                 next->pn_kid->isKind(PNK_YIELD) &&
                 next->pn_kid->pn_kid);

    RootedValue body(cx);

    return expression(next->pn_kid->pn_kid, &body) &&
           builder.generatorExpression(body, blocks, filter, &pn->pn_pos, dst);
}

bool
ASTSerializer::expression(ParseNode *pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_FUNCTION:
        return function(pn, AST_FUNC_EXPR, dst);

      case PNK_COMMA:
      {
        NodeVector exprs(cx);
        return expressions(pn, exprs) &&
               builder.sequenceExpression(exprs, &pn->pn_pos, dst);
      }

      case PNK_CONDITIONAL:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid1->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid2->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid3->pn_pos));

        RootedValue test(cx), cons(cx), alt(cx);

        return expression(pn->pn_kid1, &test) &&
               expression(pn->pn_kid2, &cons) &&
               expression(pn->pn_kid3, &alt) &&
               builder.conditionalExpression(test, cons, alt, &pn->pn_pos, dst);
      }

      case PNK_OR:
      case PNK_AND:
      {
        if (pn->isArity(PN_BINARY)) {
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

            RootedValue left(cx), right(cx);
            return expression(pn->pn_left, &left) &&
                   expression(pn->pn_right, &right) &&
                   builder.logicalExpression(pn->isKind(PNK_OR), left, right, &pn->pn_pos, dst);
        }
        return leftAssociate(pn, dst);
      }

      case PNK_PREINCREMENT:
      case PNK_PREDECREMENT:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(PNK_PREINCREMENT);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, true, &pn->pn_pos, dst);
      }

      case PNK_POSTINCREMENT:
      case PNK_POSTDECREMENT:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        bool inc = pn->isKind(PNK_POSTINCREMENT);
        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.updateExpression(expr, inc, false, &pn->pn_pos, dst);
      }

      case PNK_ASSIGN:
      case PNK_ADDASSIGN:
      case PNK_SUBASSIGN:
      case PNK_BITORASSIGN:
      case PNK_BITXORASSIGN:
      case PNK_BITANDASSIGN:
      case PNK_LSHASSIGN:
      case PNK_RSHASSIGN:
      case PNK_URSHASSIGN:
      case PNK_MULASSIGN:
      case PNK_DIVASSIGN:
      case PNK_MODASSIGN:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        AssignmentOperator op = aop(pn->getOp());
        LOCAL_ASSERT(op > AOP_ERR && op < AOP_LIMIT);

        RootedValue lhs(cx), rhs(cx);
        return pattern(pn->pn_left, NULL, &lhs) &&
               expression(pn->pn_right, &rhs) &&
               builder.assignmentExpression(op, lhs, rhs, &pn->pn_pos, dst);
      }

      case PNK_ADD:
      case PNK_SUB:
      case PNK_STRICTEQ:
      case PNK_EQ:
      case PNK_STRICTNE:
      case PNK_NE:
      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_STAR:
      case PNK_DIV:
      case PNK_MOD:
      case PNK_BITOR:
      case PNK_BITXOR:
      case PNK_BITAND:
      case PNK_IN:
      case PNK_INSTANCEOF:
      case PNK_DBLDOT:
        if (pn->isArity(PN_BINARY)) {
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

            BinaryOperator op = binop(pn->getKind(), pn->getOp());
            LOCAL_ASSERT(op > BINOP_ERR && op < BINOP_LIMIT);

            RootedValue left(cx), right(cx);
            return expression(pn->pn_left, &left) &&
                   expression(pn->pn_right, &right) &&
                   builder.binaryExpression(op, left, right, &pn->pn_pos, dst);
        }
        return leftAssociate(pn, dst);

      case PNK_DELETE:
      case PNK_TYPEOF:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG: {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        UnaryOperator op = unop(pn->getKind(), pn->getOp());
        LOCAL_ASSERT(op > UNOP_ERR && op < UNOP_LIMIT);

        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.unaryExpression(op, expr, &pn->pn_pos, dst);
      }

      case PNK_NEW:
      case PNK_CALL:
      {
#if JS_HAS_GENERATOR_EXPRS
        if (pn->isGeneratorExpr())
            return generatorExpression(pn->generatorExpr(), dst);
#endif

        ParseNode *next = pn->pn_head;
        JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

        RootedValue callee(cx);
        if (!expression(next, &callee))
            return false;

        NodeVector args(cx);
        if (!args.reserve(pn->pn_count - 1))
            return false;

        for (next = next->pn_next; next; next = next->pn_next) {
            JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue arg(cx);
            if (!expression(next, &arg))
                return false;
            args.infallibleAppend(arg);
        }

        return pn->isKind(PNK_NEW)
               ? builder.newExpression(callee, args, &pn->pn_pos, dst)

            : builder.callExpression(callee, args, &pn->pn_pos, dst);
      }

      case PNK_DOT:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

        RootedValue expr(cx), id(cx);
        RootedAtom pnAtom(cx, pn->pn_atom);
        return expression(pn->pn_expr, &expr) &&
               identifier(pnAtom, NULL, &id) &&
               builder.memberExpression(false, expr, id, &pn->pn_pos, dst);
      }

      case PNK_ELEM:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue left(cx), right(cx);
        return expression(pn->pn_left, &left) &&
               expression(pn->pn_right, &right) &&
               builder.memberExpression(true, left, right, &pn->pn_pos, dst);
      }

      case PNK_ARRAY:
      {
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
            JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            if (next->isKind(PNK_COMMA) && next->pn_count == 0) {
                elts.infallibleAppend(NullValue());
            } else {
                RootedValue expr(cx);
                if (!expression(next, &expr))
                    return false;
                elts.infallibleAppend(expr);
            }
        }

        return builder.arrayExpression(elts, &pn->pn_pos, dst);
      }

      case PNK_SPREAD:
      {
          RootedValue expr(cx);
          return expression(pn->pn_kid, &expr) &&
                 builder.spreadExpression(expr, &pn->pn_pos, dst);
      }

      case PNK_OBJECT:
      {
        /* The parser notes any uninitialized properties by setting the PNX_DESTRUCT flag. */
        if (pn->pn_xflags & PNX_DESTRUCT) {
            parser->reportError(pn, JSMSG_BAD_OBJECT_INIT);
            return false;
        }
        NodeVector elts(cx);
        if (!elts.reserve(pn->pn_count))
            return false;

        for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
            JS_ASSERT(pn->pn_pos.encloses(next->pn_pos));

            RootedValue prop(cx);
            if (!property(next, &prop))
                return false;
            elts.infallibleAppend(prop);
        }

        return builder.objectExpression(elts, &pn->pn_pos, dst);
      }

      case PNK_NAME:
        return identifier(pn, dst);

      case PNK_THIS:
        return builder.thisExpression(&pn->pn_pos, dst);

      case PNK_STRING:
      case PNK_REGEXP:
      case PNK_NUMBER:
      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_NULL:
        return literal(pn, dst);

      case PNK_YIELD:
      {
        JS_ASSERT_IF(pn->pn_kid, pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue arg(cx);
        return optExpression(pn->pn_kid, &arg) &&
               builder.yieldExpression(arg, &pn->pn_pos, dst);
      }

      case PNK_ARRAYCOMP:
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_head->pn_pos));

        /* NB: it's no longer the case that pn_count could be 2. */
        LOCAL_ASSERT(pn->pn_count == 1);
        LOCAL_ASSERT(pn->pn_head->isKind(PNK_LEXICALSCOPE));

        return comprehension(pn->pn_head->pn_expr, dst);

      case PNK_LET:
        return let(pn, true, dst);

#if JS_HAS_XML_SUPPORT
      case PNK_XMLUNARY:
        JS_ASSERT(pn->isOp(JSOP_XMLNAME) ||
                  pn->isOp(JSOP_SETXMLNAME) ||
                  pn->isOp(JSOP_BINDXMLNAME));
        return expression(pn->pn_kid, dst);

      case PNK_ANYNAME:
        return builder.xmlAnyName(&pn->pn_pos, dst);

      case PNK_DBLCOLON:
      {
        RootedValue right(cx);

        LOCAL_ASSERT(pn->isArity(PN_NAME) || pn->isArity(PN_BINARY));

        ParseNode *pnleft;
        bool computed;

        if (pn->isArity(PN_BINARY)) {
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

            computed = true;
            pnleft = pn->pn_left;
            if (!expression(pn->pn_right, &right))
                return false;
        } else {
            JS_ASSERT(pn->isArity(PN_NAME));
            JS_ASSERT(pn->pn_pos.encloses(pn->pn_expr->pn_pos));

            computed = false;
            pnleft = pn->pn_expr;
            RootedAtom pnAtom(cx, pn->pn_atom);
            if (!identifier(pnAtom, NULL, &right))
                return false;
        }

        if (pnleft->isKind(PNK_FUNCTIONNS))
            return builder.xmlFunctionQualifiedIdentifier(right, computed, &pn->pn_pos, dst);

        RootedValue left(cx);
        return expression(pnleft, &left) &&
               builder.xmlQualifiedIdentifier(left, right, computed, &pn->pn_pos, dst);
      }

      case PNK_AT:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue expr(cx);
        ParseNode *kid = pn->pn_kid;
        bool computed = ((!kid->isKind(PNK_NAME) || !kid->isOp(JSOP_QNAMEPART)) &&
                         !kid->isKind(PNK_DBLCOLON) &&
                         !kid->isKind(PNK_ANYNAME));
        return expression(kid, &expr) &&
            builder.xmlAttributeSelector(expr, computed, &pn->pn_pos, dst);
      }

      case PNK_FILTER:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_left->pn_pos));
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_right->pn_pos));

        RootedValue left(cx), right(cx);
        return expression(pn->pn_left, &left) &&
               expression(pn->pn_right, &right) &&
               builder.xmlFilterExpression(left, right, &pn->pn_pos, dst);
      }

      default:
        return xml(pn, dst);

#else
      default:
        LOCAL_NOT_REACHED("unexpected expression type");
#endif
    }
}

bool
ASTSerializer::xml(ParseNode *pn, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
#if JS_HAS_XML_SUPPORT
      case PNK_XMLCURLYEXPR:
      {
        JS_ASSERT(pn->pn_pos.encloses(pn->pn_kid->pn_pos));

        RootedValue expr(cx);
        return expression(pn->pn_kid, &expr) &&
               builder.xmlEscapeExpression(expr, &pn->pn_pos, dst);
      }

      case PNK_XMLELEM:
      {
        NodeVector elts(cx);
        if (!xmls(pn, elts))
            return false;
        return builder.xmlElement(elts, &pn->pn_pos, dst);
      }

      case PNK_XMLLIST:
      {
        NodeVector elts(cx);
        if (!xmls(pn, elts))
            return false;
        return builder.xmlList(elts, &pn->pn_pos, dst);
      }

      case PNK_XMLSTAGO:
      {
        NodeVector elts(cx);
        if (!xmls(pn, elts))
            return false;
        return builder.xmlStartTag(elts, &pn->pn_pos, dst);
      }

      case PNK_XMLETAGO:
      {
        NodeVector elts(cx);
        if (!xmls(pn, elts))
            return false;
        return builder.xmlEndTag(elts, &pn->pn_pos, dst);
      }

      case PNK_XMLPTAGC:
      {
        NodeVector elts(cx);
        if (!xmls(pn, elts))
            return false;
        return builder.xmlPointTag(elts, &pn->pn_pos, dst);
      }

      case PNK_XMLTEXT:
      case PNK_XMLSPACE: {
        RootedValue atomContentsVal(cx, unrootedAtomContents(pn->pn_atom));
        return builder.xmlText(atomContentsVal, &pn->pn_pos, dst);
      }

      case PNK_XMLNAME:
        if (pn->isArity(PN_NULLARY)) {
            RootedValue atomContentsVal(cx, unrootedAtomContents(pn->pn_atom));
            return builder.xmlName(atomContentsVal, &pn->pn_pos, dst);
        }

        LOCAL_ASSERT(pn->isArity(PN_LIST));

        {
            NodeVector elts(cx);
            return xmls(pn, elts) &&
                   builder.xmlName(elts, &pn->pn_pos, dst);
        }

      case PNK_XMLATTR: {
        RootedValue atomContentsVal(cx, unrootedAtomContents(pn->pn_atom));
        return builder.xmlAttribute(atomContentsVal, &pn->pn_pos, dst);
      }

      case PNK_XMLCDATA: {
        RootedValue atomContentsVal(cx, unrootedAtomContents(pn->pn_atom));
        return builder.xmlCdata(atomContentsVal, &pn->pn_pos, dst);
      }

      case PNK_XMLCOMMENT: {
        RootedValue atomContentsVal(cx, unrootedAtomContents(pn->pn_atom));
        return builder.xmlComment(atomContentsVal, &pn->pn_pos, dst);
      }

      case PNK_XMLPI: {
        XMLProcessingInstruction &pi = pn->as<XMLProcessingInstruction>();
        RootedValue targetAtomContentsVal(cx, unrootedAtomContents(pi.target()));
        RootedValue dataAtomContentsVal(cx, unrootedAtomContents(pi.data()));
        return builder.xmlPI(targetAtomContentsVal,
                             dataAtomContentsVal,
                             &pi.pn_pos,
                             dst);
      }
#endif

      default:
        LOCAL_NOT_REACHED("unexpected XML node type");
    }
}

bool
ASTSerializer::propertyName(ParseNode *pn, MutableHandleValue dst)
{
    if (pn->isKind(PNK_NAME))
        return identifier(pn, dst);

    LOCAL_ASSERT(pn->isKind(PNK_STRING) || pn->isKind(PNK_NUMBER));

    return literal(pn, dst);
}

bool
ASTSerializer::property(ParseNode *pn, MutableHandleValue dst)
{
    PropKind kind;
    switch (pn->getOp()) {
      case JSOP_INITPROP:
        kind = PROP_INIT;
        break;

      case JSOP_GETTER:
        kind = PROP_GETTER;
        break;

      case JSOP_SETTER:
        kind = PROP_SETTER;
        break;

      default:
        LOCAL_NOT_REACHED("unexpected object-literal property");
    }

    RootedValue key(cx), val(cx);
    return propertyName(pn->pn_left, &key) &&
           expression(pn->pn_right, &val) &&
           builder.propertyInitializer(key, val, kind, &pn->pn_pos, dst);
}

bool
ASTSerializer::literal(ParseNode *pn, MutableHandleValue dst)
{
    RootedValue val(cx);
    switch (pn->getKind()) {
      case PNK_STRING:
        val.setString(pn->pn_atom);
        break;

      case PNK_REGEXP:
      {
        RootedObject re1(cx, pn->pn_objbox ? pn->pn_objbox->object : NULL);
        LOCAL_ASSERT(re1 && re1->isRegExp());

        RootedObject proto(cx);
        if (!js_GetClassPrototype(cx, JSProto_RegExp, &proto))
            return false;

        RootedObject re2(cx, CloneRegExpObject(cx, re1, proto));
        if (!re2)
            return false;

        val.setObject(*re2);
        break;
      }

      case PNK_NUMBER:
        val.setNumber(pn->pn_dval);
        break;

      case PNK_NULL:
        val.setNull();
        break;

      case PNK_TRUE:
        val.setBoolean(true);
        break;

      case PNK_FALSE:
        val.setBoolean(false);
        break;

      default:
        LOCAL_NOT_REACHED("unexpected literal type");
    }

    return builder.literal(val, &pn->pn_pos, dst);
}

bool
ASTSerializer::arrayPattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst)
{
    JS_ASSERT(pn->isKind(PNK_ARRAY));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        /* Comma expressions can't occur inside patterns, so no need to test pn_count. */
        JS_ASSERT_IF(next->isKind(PNK_COMMA), next->pn_count == 0);

        if (next->isKind(PNK_COMMA)) {
            elts.infallibleAppend(NullValue());
        } else {
            RootedValue patt(cx);
            if (!pattern(next, pkind, &patt))
                return false;
            elts.infallibleAppend(patt);
        }
    }

    return builder.arrayPattern(elts, &pn->pn_pos, dst);
}

bool
ASTSerializer::objectPattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst)
{
    JS_ASSERT(pn->isKind(PNK_OBJECT));

    NodeVector elts(cx);
    if (!elts.reserve(pn->pn_count))
        return false;

    for (ParseNode *next = pn->pn_head; next; next = next->pn_next) {
        LOCAL_ASSERT(next->isOp(JSOP_INITPROP));

        RootedValue key(cx), patt(cx), prop(cx);
        if (!propertyName(next->pn_left, &key) ||
            !pattern(next->pn_right, pkind, &patt) ||
            !builder.propertyPattern(key, patt, &next->pn_pos, &prop)) {
            return false;
        }

        elts.infallibleAppend(prop);
    }

    return builder.objectPattern(elts, &pn->pn_pos, dst);
}

bool
ASTSerializer::pattern(ParseNode *pn, VarDeclKind *pkind, MutableHandleValue dst)
{
    JS_CHECK_RECURSION(cx, return false);
    switch (pn->getKind()) {
      case PNK_OBJECT:
        return objectPattern(pn, pkind, dst);

      case PNK_ARRAY:
        return arrayPattern(pn, pkind, dst);

      case PNK_NAME:
        if (pkind && (pn->pn_dflags & PND_CONST))
            *pkind = VARDECL_CONST;
        /* FALL THROUGH */

      default:
        return expression(pn, dst);
    }
}

bool
ASTSerializer::identifier(HandleAtom atom, TokenPos *pos, MutableHandleValue dst)
{
    RootedValue atomContentsVal(cx, unrootedAtomContents(atom));
    return builder.identifier(atomContentsVal, pos, dst);
}

bool
ASTSerializer::identifier(ParseNode *pn, MutableHandleValue dst)
{
    LOCAL_ASSERT(pn->isArity(PN_NAME) || pn->isArity(PN_NULLARY));
    LOCAL_ASSERT(pn->pn_atom);

    RootedAtom pnAtom(cx, pn->pn_atom);
    return identifier(pnAtom, &pn->pn_pos, dst);
}

bool
ASTSerializer::function(ParseNode *pn, ASTType type, MutableHandleValue dst)
{
    RootedFunction func(cx, pn->pn_funbox->function());

    bool isGenerator =
#if JS_HAS_GENERATORS
        pn->pn_funbox->isGenerator();
#else
        false;
#endif

    bool isExpression =
#if JS_HAS_EXPR_CLOSURES
        func->isExprClosure();
#else
        false;
#endif

    RootedValue id(cx);
    RootedAtom funcAtom(cx, func->atom());
    if (!optIdentifier(funcAtom, NULL, &id))
        return false;

    NodeVector args(cx);
    NodeVector defaults(cx);

    RootedValue body(cx), rest(cx);
    if (func->hasRest())
        rest.setUndefined();
    else
        rest.setNull();
    return functionArgsAndBody(pn->pn_body, args, defaults, &body, &rest) &&
        builder.function(type, &pn->pn_pos, id, args, defaults, body,
                         rest, isGenerator, isExpression, dst);
}

bool
ASTSerializer::functionArgsAndBody(ParseNode *pn, NodeVector &args, NodeVector &defaults,
                                   MutableHandleValue body, MutableHandleValue rest)
{
    ParseNode *pnargs;
    ParseNode *pnbody;

    /* Extract the args and body separately. */
    if (pn->isKind(PNK_ARGSBODY)) {
        pnargs = pn;
        pnbody = pn->last();
    } else {
        pnargs = NULL;
        pnbody = pn;
    }

    ParseNode *pndestruct;

    /* Extract the destructuring assignments. */
    if (pnbody->isArity(PN_LIST) && (pnbody->pn_xflags & PNX_DESTRUCT)) {
        ParseNode *head = pnbody->pn_head;
        LOCAL_ASSERT(head && head->isKind(PNK_SEMI));

        pndestruct = head->pn_kid;
        LOCAL_ASSERT(pndestruct);
        LOCAL_ASSERT(pndestruct->isKind(PNK_VAR));
    } else {
        pndestruct = NULL;
    }

    /* Serialize the arguments and body. */
    switch (pnbody->getKind()) {
      case PNK_RETURN: /* expression closure, no destructured args */
        return functionArgs(pn, pnargs, NULL, pnbody, args, defaults, rest) &&
               expression(pnbody->pn_kid, body);

      case PNK_SEQ:    /* expression closure with destructured args */
      {
        ParseNode *pnstart = pnbody->pn_head->pn_next;
        LOCAL_ASSERT(pnstart && pnstart->isKind(PNK_RETURN));

        return functionArgs(pn, pnargs, pndestruct, pnbody, args, defaults, rest) &&
               expression(pnstart->pn_kid, body);
      }

      case PNK_STATEMENTLIST:     /* statement closure */
      {
        ParseNode *pnstart = (pnbody->pn_xflags & PNX_DESTRUCT)
                               ? pnbody->pn_head->pn_next
                               : pnbody->pn_head;

        return functionArgs(pn, pnargs, pndestruct, pnbody, args, defaults, rest) &&
               functionBody(pnstart, &pnbody->pn_pos, body);
      }

      default:
        LOCAL_NOT_REACHED("unexpected function contents");
    }
}

bool
ASTSerializer::functionArgs(ParseNode *pn, ParseNode *pnargs, ParseNode *pndestruct,
                            ParseNode *pnbody, NodeVector &args, NodeVector &defaults,
                            MutableHandleValue rest)
{
    uint32_t i = 0;
    ParseNode *arg = pnargs ? pnargs->pn_head : NULL;
    ParseNode *destruct = pndestruct ? pndestruct->pn_head : NULL;
    RootedValue node(cx);

    /*
     * Arguments are found in potentially two different places: 1) the
     * argsbody sequence (which ends with the body node), or 2) a
     * destructuring initialization at the beginning of the body. Loop
     * |arg| through the argsbody and |destruct| through the initial
     * destructuring assignments, stopping only when we've exhausted
     * both.
     */
    while ((arg && arg != pnbody) || destruct) {
        if (destruct && destruct->pn_right->frameSlot() == i) {
            if (!pattern(destruct->pn_left, NULL, &node) || !args.append(node))
                return false;
            destruct = destruct->pn_next;
        } else if (arg && arg != pnbody) {
            /*
             * We don't check that arg->frameSlot() == i since we
             * can't call that method if the arg def has been turned
             * into a use, e.g.:
             *
             *     function(a) { function a() { } }
             *
             * There's no other way to ask a non-destructuring arg its
             * index in the formals list, so we rely on the ability to
             * ask destructuring args their index above.
             */
            JS_ASSERT(arg->isKind(PNK_NAME) || arg->isKind(PNK_ASSIGN));
            ParseNode *argName = arg->isKind(PNK_NAME) ? arg : arg->pn_left;
            if (!identifier(argName, &node))
                return false;
            if (rest.isUndefined() && arg->pn_next == pnbody)
                rest.setObject(node.toObject());
            else if (!args.append(node))
                return false;
            if (arg->pn_dflags & PND_DEFAULT) {
                ParseNode *expr = arg->isDefn() ? arg->expr() : arg->pn_kid->pn_right;
                RootedValue def(cx);
                if (!expression(expr, &def) || !defaults.append(def))
                    return false;
            }
            arg = arg->pn_next;
        } else {
            LOCAL_NOT_REACHED("missing function argument");
        }
        ++i;
    }
    JS_ASSERT(!rest.isUndefined());

    return true;
}

bool
ASTSerializer::functionBody(ParseNode *pn, TokenPos *pos, MutableHandleValue dst)
{
    NodeVector elts(cx);

    /* We aren't sure how many elements there are up front, so we'll check each append. */
    for (ParseNode *next = pn; next; next = next->pn_next) {
        RootedValue child(cx);
        if (!sourceElement(next, &child) || !elts.append(child))
            return false;
    }

    return builder.blockStatement(elts, pos, dst);
}

} /* namespace js */

static JSBool
reflect_parse(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc < 1) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "Reflect.parse", "0", "s");
        return JS_FALSE;
    }

    RootedString src(cx, ToString(cx, JS_ARGV(cx, vp)[0]));
    if (!src)
        return JS_FALSE;

    char *filename = NULL;
    AutoReleaseNullablePtr filenamep(filename);
    uint32_t lineno = 1;
    bool loc = true;

    RootedObject builder(cx);

    RootedValue arg(cx, argc > 1 ? JS_ARGV(cx, vp)[1] : UndefinedValue());

    if (!arg.isNullOrUndefined()) {
        if (!arg.isObject()) {
            js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                     JSDVG_SEARCH_STACK, arg, NullPtr(), "not an object", NULL);
            return JS_FALSE;
        }

        RootedObject config(cx, &arg.toObject());

        RootedValue prop(cx);

        /* config.loc */
        RootedId locId(cx, NameToId(cx->names().loc));
        RootedValue trueVal(cx, BooleanValue(true));
        if (!baseops::GetPropertyDefault(cx, config, locId, trueVal, &prop))
            return JS_FALSE;

        loc = ToBoolean(prop);

        if (loc) {
            /* config.source */
            RootedId sourceId(cx, NameToId(cx->names().source));
            RootedValue nullVal(cx, NullValue());
            if (!baseops::GetPropertyDefault(cx, config, sourceId, nullVal, &prop))
                return JS_FALSE;

            if (!prop.isNullOrUndefined()) {
                RootedString str(cx, ToString(cx, prop));
                if (!str)
                    return JS_FALSE;

                size_t length = str->length();
                const jschar *chars = str->getChars(cx);
                if (!chars)
                    return JS_FALSE;

                filename = DeflateString(cx, chars, length);
                if (!filename)
                    return JS_FALSE;
                filenamep.reset(filename);
            }

            /* config.line */
            RootedId lineId(cx, NameToId(cx->names().line));
            RootedValue oneValue(cx, Int32Value(1));
            if (!baseops::GetPropertyDefault(cx, config, lineId, oneValue, &prop) ||
                !ToUint32(cx, prop, &lineno)) {
                return JS_FALSE;
            }
        }

        /* config.builder */
        RootedId builderId(cx, NameToId(cx->names().builder));
        RootedValue nullVal(cx, NullValue());
        if (!baseops::GetPropertyDefault(cx, config, builderId, nullVal, &prop))
            return JS_FALSE;

        if (!prop.isNullOrUndefined()) {
            if (!prop.isObject()) {
                js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                         JSDVG_SEARCH_STACK, prop, NullPtr(), "not an object", NULL);
                return JS_FALSE;
            }
            builder = &prop.toObject();
        }
    }

    /* Extract the builder methods first to report errors before parsing. */
    ASTSerializer serialize(cx, loc, filename, lineno);
    if (!serialize.init(builder))
        return JS_FALSE;

    JSStableString *stable = src->ensureStable(cx);
    if (!stable)
        return JS_FALSE;

    const StableCharPtr chars = stable->chars();
    size_t length = stable->length();
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);
    Parser parser(cx, options, chars.get(), length, /* foldConstants = */ false);
    if (!parser.init())
        return JS_FALSE;

    serialize.setParser(&parser);

    ParseNode *pn = parser.parse(NULL);
    if (!pn)
        return JS_FALSE;

    RootedValue val(cx);
    if (!serialize.program(pn, &val)) {
        JS_SET_RVAL(cx, vp, JSVAL_NULL);
        return JS_FALSE;
    }

    JS_SET_RVAL(cx, vp, val);
    return JS_TRUE;
}

static JSFunctionSpec static_methods[] = {
    JS_FN("parse", reflect_parse, 1, 0),
    JS_FS_END
};


JS_PUBLIC_API(JSObject *)
JS_InitReflect(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    RootedObject Reflect(cx, NewObjectWithClassProto(cx, &ObjectClass, NULL, obj));
    if (!Reflect || !JSObject::setSingletonType(cx, Reflect))
        return NULL;

    if (!JS_DefineProperty(cx, obj, "Reflect", OBJECT_TO_JSVAL(Reflect),
                           JS_PropertyStub, JS_StrictPropertyStub, 0)) {
        return NULL;
    }

    if (!JS_DefineFunctions(cx, Reflect, static_methods))
        return NULL;

    return Reflect;
}
