//
//  File: %d-gc.c
//  Summary: "Debug-Build Checks for the Garbage Collector"
//  Section: debug
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The R3-Alpha GC had to do switch() on the kind of cell to know how to
// handle it.  Ren-C makes bits in the value cell itself dictate what needs
// to be done...which is faster, but it doesn't get the benefit of checking
// additional invariants that the switch() branches were doing.
//
// This file extracts the switch()-based checks so that they do not clutter
// the readability of the main GC code.
//

#include "sys-core.h"

#if !defined(NDEBUG)

#define Is_Marked(n) \
    (SER(n)->header.bits & NODE_FLAG_MARKED)


//
//  Assert_Cell_Marked_Correctly: C
//
// Note: We assume the binding was marked correctly if the type was bindable.
//
void Assert_Cell_Marked_Correctly(const RELVAL *quotable)
{
    const REBCEL *v;  // do GC work on this variable, not quoted
    enum Reb_Kind kind;
    if (KIND_BYTE_UNCHECKED(quotable) != REB_QUOTED) {
        kind = CELL_KIND_UNCHECKED(cast(const REBCEL*, quotable)); // mod 64
        v = quotable;
    }
    else {
        v = VAL_QUOTED_PAYLOAD_CELL(quotable);
        assert(v->header.bits & NODE_FLAG_MARKED);
        if (Is_Bindable(v))
            assert(EXTRA(Binding, v).node == EXTRA(Binding, quotable).node);
        else {
            assert(EXTRA(Binding, quotable).node == nullptr);
            // Note: Unbindable cell bits can be used for whatever they like
        }
        assert(KIND_BYTE_UNCHECKED(v) < REB_MAX);
        kind = cast(enum Reb_Kind, KIND_BYTE_UNCHECKED(v));
    }

    if (IS_BINDABLE_KIND(kind)) {
        REBNOD *binding = VAL_BINDING(v);
        if (
            binding
            and not (binding->header.bits & NODE_FLAG_MANAGED)
            and NOT_CELL_FLAG(v, STACK_LIFETIME)
            and NOT_CELL_FLAG(v, TRANSIENT)
        ){
            // If a stack cell holds an unmanaged stack-based pointer, we
            // assume the lifetime is taken care of and the GC does not need
            // to be involved.  Only stack cells are allowed to do this.
            //
            panic (v);
        }
    }

    // This switch was originally done via contiguous REB_XXX values, in order
    // to facilitate use of a "jump table optimization":
    //
    // http://stackoverflow.com/questions/17061967/c-switch-and-jump-tables
    //
    // Since this is debug-only, it's not as important any more.  But it
    // still can speed things up to go in order.
    //
    switch (kind) {
      case REB_0_END:
      case REB_NULLED:
      case REB_VOID:
      case REB_BLANK:
        break;

      case REB_LOGIC:
      case REB_INTEGER:
      case REB_DECIMAL:
      case REB_PERCENT:
      case REB_MONEY:
        break;

      case REB_CHAR:
        assert(VAL_CHAR_ENCODED_SIZE(v) <= 4);
        break;

      case REB_PAIR: {
        REBVAL *p = PAYLOAD(Pair, v).paired;
        assert(Is_Marked(p));
        break; }

      case REB_TUPLE:
      case REB_TIME:
      case REB_DATE:
        break;

      case REB_DATATYPE:
        // Type spec is allowed to be NULL.  See %typespec.r file
        if (VAL_TYPE_SPEC(v))
            assert(Is_Marked(VAL_TYPE_SPEC(v)));
        break;

      case REB_TYPESET: // !!! Currently just 64-bits of bitset
        break;

      case REB_BITSET: {
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBSER *s = SER(PAYLOAD(Any, v).first.node);
        if (GET_SERIES_INFO(s, INACCESSIBLE))
            assert(Is_Marked(s));  // TBD: clear out reference and GC `s`?
        else
            assert(Is_Marked(s));
        break; }

      case REB_MAP: {
        REBMAP* map = VAL_MAP(v);
        assert(Is_Marked(map));
        break; }

      case REB_HANDLE: { // See %sys-handle.h
        REBARR *a = EXTRA(Handle, v).singular;
        if (not a) {
            //
            // This HANDLE! was created with Init_Handle_Simple.  There is
            // no GC interaction.
        }
        else {
            // Handle was created with Init_Handle_Managed.  It holds a
            // REBSER node that contains exactly one handle, and the actual
            // data for the handle lives in that shared location.  There is
            // nothing the GC needs to see inside a handle.
            //
            assert(Is_Marked(a));

            assert(ARR_LEN(a) == 1);
            RELVAL *single = ARR_SINGLE(a);
            assert(IS_HANDLE(single));
            assert(EXTRA(Handle, single).singular == a);
            if (v != single) {
                //
                // In order to make it clearer that individual handles do not
                // hold the shared data (there'd be no way to update all the
                // references at once), the data pointers in all but the
                // shared singular value are NULL.
                //
                if (Is_Handle_Cfunc(v))
                    assert(
                        IS_CFUNC_TRASH_DEBUG(PAYLOAD(Handle,v).data.cfunc)
                    );
                else
                    assert(
                        IS_POINTER_TRASH_DEBUG(PAYLOAD(Handle,v).data.pointer)
                    );
            }
        }
        break; }


      case REB_LIBRARY: {
        assert(Is_Marked(VAL_LIBRARY(v)));
        REBCTX *meta = VAL_LIBRARY_META(v);
        if (meta)
            assert(Is_Marked(meta));
        break; }

    //=//// CUSTOM EXTENSION TYPES ////////////////////////////////////////=//

      case REB_GOB: {  // 7-element REBARR
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBARR *gob = ARR(PAYLOAD(Any, v).first.node);
        assert(GET_SERIES_INFO(gob, LINK_IS_CUSTOM_NODE));
        assert(GET_SERIES_INFO(gob, MISC_IS_CUSTOM_NODE));
        assert(Is_Marked(PAYLOAD(Any, v).first.node));
        break; }

      case REB_EVENT: {  // packed cell structure with one GC-able slot
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBNOD *n = PAYLOAD(Any, v).first.node;  // REBGOB*, REBREQ*, etc.
        assert(n == nullptr or n->header.bits & NODE_FLAG_NODE);
        assert(Is_Marked(PAYLOAD(Any, v).first.node));
        break; }

      case REB_STRUCT: {  // like an OBJECT!, but the "varlist" can be binary
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBSER *data = SER(PAYLOAD(Any, v).first.node);
        assert(BYTE_SIZE(data) or IS_SER_ARRAY(data));
        assert(Is_Marked(PAYLOAD(Any, v).first.node));
        break; }

      case REB_IMAGE: {  // currently a 3-element array (could be a pairing)
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBARR *arr = ARR(PAYLOAD(Any, v).first.node);
        assert(ARR_LEN(arr) == 1);
        assert(NOT_SERIES_INFO(arr, LINK_IS_CUSTOM_NODE));  // stores width
        assert(NOT_SERIES_INFO(arr, MISC_IS_CUSTOM_NODE));  // stores hieght
        assert(Is_Marked(PAYLOAD(Any, v).first.node));
        break; }

      case REB_VECTOR: {  // currently a pairing (BINARY! and an info cell)
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBVAL *p = VAL(PAYLOAD(Any, v).first.node);
        assert(IS_BINARY(p));
        assert(KIND_BYTE(PAIRING_KEY(p)) == REB_V_SIGN_INTEGRAL_WIDE);
        assert(Is_Marked(PAYLOAD(Any, v).first.node));
        break; }

      case REB_BINARY: {
        REBBIN *s = SER(PAYLOAD(Any, v).first.node);
        assert(SER_WIDE(s) == sizeof(REBYTE));
        if (GET_SERIES_INFO(s, INACCESSIBLE))
            assert(Is_Marked(s));  // TBD: clear out reference and GC `s`?
        else {
            ASSERT_SERIES_TERM(s);
            assert(Is_Marked(s));
        }
        break; }

      case REB_TEXT:
      case REB_FILE:
      case REB_EMAIL:
      case REB_URL:
      case REB_TAG: {
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBSER *s = SER(PAYLOAD(Any, v).first.node);
        assert(SER_WIDE(s) == sizeof(REBYTE));
        assert(GET_SERIES_FLAG(s, UTF8_NONWORD));  // !!! temporary
        if (GET_SERIES_INFO(s, INACCESSIBLE))
            assert(Is_Marked(s));  // TBD: clear out reference and GC `s`?
        else {
            ASSERT_SERIES_TERM(s);
            assert(Is_Marked(s));
        }
        REBBMK *bookmark = LINK(s).bookmarks;
        if (bookmark) {
            assert(not LINK(bookmark).bookmarks);  // just one for now
            //
            // The intent is that bookmarks are unmanaged REBSERs, which
            // get freed when the string GCs.  This mechanic could be a by
            // product of noticing that the SERIES_INFO_LINK_IS_NODE is true
            // but that the managed bit on the node is false.

            assert(not Is_Marked(bookmark));
            assert(NOT_SERIES_FLAG(bookmark, MANAGED));
        }
        break; }

    //=//// BEGIN BINDABLE TYPES ////////////////////////////////////////=//

      case REB_ISSUE:
          goto any_word;  // !!! this is being changed back to ANY-STRING!

      case REB_OBJECT:
      case REB_MODULE:
      case REB_ERROR:
      case REB_FRAME:
      case REB_PORT: {  // Note: VAL_CONTEXT() fails on SER_INFO_INACCESSIBLE
        REBCTX *context = CTX(PAYLOAD(Any, v).first.node);
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        assert(Is_Marked(context));

        // Currently the "binding" in a context is only used by FRAME! to
        // preserve the binding of the ACTION! value that spawned that
        // frame.  Currently that binding is typically NULL inside of a
        // function's REBVAL unless it is a definitional RETURN or LEAVE.
        //
        // !!! Expanded usages may be found in other situations that mix an
        // archetype with an instance (e.g. an archetypal function body that
        // could apply to any OBJECT!, but the binding cheaply makes it
        // a method for that object.)
        //
        if (EXTRA(Binding, v).node != UNBOUND) {
            assert(CTX_TYPE(context) == REB_FRAME);

            if (GET_SERIES_INFO(context, INACCESSIBLE)) {
                //
                // !!! It seems a bit wasteful to keep alive the binding of a
                // stack frame you can no longer get values out of.  But
                // However, FUNCTION-OF still works on a FRAME! value after
                // the function is finished, if the FRAME! value was kept.
                // And that needs to give back a correct binding.
                //
            }
            else {
                struct Reb_Frame *f = CTX_FRAME_IF_ON_STACK(context);
                if (f) // comes from execution, not MAKE FRAME!
                    assert(VAL_BINDING(v) == FRM_BINDING(f));
            }
        }

        REBACT *phase = ACT(PAYLOAD(Any, v).second.node);
        if (phase) {
            assert(kind == REB_FRAME); // may be heap-based frame
            assert(Is_Marked(phase));
        }
        else
            assert(kind != REB_FRAME); // phase if-and-only-if frame

        if (GET_SERIES_INFO(context, INACCESSIBLE))
            break;

        REBVAL *archetype = CTX_ARCHETYPE(context);
        assert(CTX_TYPE(context) == kind);
        assert(VAL_CONTEXT(archetype) == context);

        // Note: for VAL_CONTEXT_FRAME, the FRM_CALL is either on the stack
        // (in which case it's already taken care of for marking) or it
        // has gone bad, in which case it should be ignored.

        break; }

      case REB_VARARGS: {
        REBNOD *binding = VAL_BINDING(v);
        assert(IS_SER_ARRAY(binding));
        assert(
            GET_ARRAY_FLAG(binding, IS_VARLIST)
            or not IS_SER_DYNAMIC(binding)  // singular
        );

        if (PAYLOAD(Varargs, v).phase)  // null if came from MAKE VARARGS!
            assert(Is_Marked(PAYLOAD(Varargs, v).phase));
        break; }

      case REB_BLOCK:
      case REB_SET_BLOCK:
      case REB_GET_BLOCK:
      case REB_GROUP:
      case REB_SET_GROUP:
      case REB_GET_GROUP: {
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBARR *a = ARR(PAYLOAD(Any, v).first.node);
        if (GET_SERIES_INFO(a, INACCESSIBLE)) {
            //
            // !!! Review: preserving the identity of inaccessible array nodes
            // is likely uninteresting--the only reason the node wasn't freed
            // in the first place was so this code wouldn't crash trying to
            // mark it.  So this should probably be used as an opportunity to
            // update the pointer in the cell to some global inaccessible
            // REBARR, and *not* mark the dead node at all.
            //
            assert(Is_Marked(a));
        }
        else {
            assert(Is_Marked(a));
        }
        break; }

      case REB_PATH:
      case REB_SET_PATH:
      case REB_GET_PATH: {
        assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
        REBARR *a = ARR(PAYLOAD(Any, v).first.node);
        assert(NOT_SERIES_INFO(a, INACCESSIBLE));

        // With most arrays we may risk direct recursion, hence we have to
        // use Queue_Mark_Array_Deep().  But paths are guaranteed to not have
        // other paths directly in them.  Walk it here so that we can also
        // check that there are no paths embedded.
        //
        // Note: This doesn't catch cases which don't wind up reachable from
        // the root set, e.g. anything that would be GC'd.
        //
        // !!! Optimization abandoned

        assert(ARR_LEN(a) >= 2);
        RELVAL *item = ARR_HEAD(a);
        for (; NOT_END(item); ++item)
            assert(not ANY_PATH_KIND(KIND_BYTE_UNCHECKED(item)));
        assert(Is_Marked(a));
        break; }

      any_word:
      case REB_WORD:
      case REB_SET_WORD:
      case REB_GET_WORD: {
        REBSTR *spelling = STR(PAYLOAD(Any, v).first.node);

        // A word marks the specific spelling it uses, but not the canon
        // value.  That's because if the canon value gets GC'd, then
        // another value might become the new canon during that sweep.
        //
        assert(Is_Marked(spelling));

        assert(  // GC can't run during binding, only time bind indices != 0
            NOT_SERIES_INFO(spelling, STRING_CANON)
            or (
                MISC(spelling).bind_index.high == 0
                and MISC(spelling).bind_index.low == 0
            )
        );

        if (IS_WORD_BOUND(v)) {
            assert(PAYLOAD(Any, v).second.i32 > 0);
        }
        else {
            // The word is unbound...make sure index is 0 in debug build.
            // (it can be left uninitialized in release builds, for now)
            //
            assert(PAYLOAD(Any, v).second.i32 == -1);
        }
        break; }

      case REB_ACTION: {
        REBACT *a = VAL_ACTION(v);
        assert(Is_Marked(a));

        // Make sure the [0] slot of the paramlist holds an archetype that is
        // consistent with the paramlist itself.
        //
        REBVAL *archetype = ACT_ARCHETYPE(a);
        assert(ACT_PARAMLIST(a) == VAL_ACT_PARAMLIST(archetype));
        assert(ACT_DETAILS(a) == VAL_ACT_DETAILS(archetype));
        break; }

      case REB_QUOTED:
        //
        // REB_QUOTED should not be contained in a quoted; instead, the
        // depth of the existing literal should just have been incremented.
        //
        panic ("REB_QUOTED with (KIND_BYTE() % REB_64) > 0");

    //=//// BEGIN INTERNAL TYPES ////////////////////////////////////////=//

      case REB_P_NORMAL:
      case REB_P_HARD_QUOTE:
      case REB_P_SOFT_QUOTE:
      case REB_P_REFINEMENT:
      case REB_P_LOCAL:
      case REB_P_RETURN: {
        REBSTR *s = EXTRA(Key, v).spelling;
        assert(SER_WIDE(s) == 1); // UTF-8 REBSTR
        assert(Is_Marked(s));
        break; }

      case REB_G_XYF:
        //
        // This is a compact type that stores floats in the payload, and
        // miscellaneous information in the extra.  None of it needs GC
        // awareness--the cells that need GC awareness use ordinary values.
        // It's to help pack all the data needed for the GOB! into one
        // allocation and still keep it under 8 cells in size, without
        // having to get involved with using HANDLE!.
        //
        break;

      case REB_V_SIGN_INTEGRAL_WIDE:
        //
        // Similar to the above.  Since it has no GC behavior and the caller
        // knows where these cells are (stealing space in an array) there is
        // no need for a unique type, but it may help in debugging if these
        // values somehow escape their "details" arrays.
        //
        break;

      case REB_X_BOOKMARK:  // ANY-STRING! index and offset cache
        break;

      default:
        panic (v);
    }
}

#endif