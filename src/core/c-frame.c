//
//  File: %c-frame.c
//  Summary: "frame management"
//  Section: core
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
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
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
/*
        This structure is used for:

            1. Modules
            2. Objects
            3. Function frame (arguments)
            4. Closures

        A frame is a block that begins with a special FRAME! value
        (a datatype that links to the frame word list). That value
        (SELF) is followed by the values of the words for the frame.

        FRAME BLOCK:                            WORD LIST:
        +----------------------------+          +----------------------------+
        |    Frame Datatype Value    |--Series->|         SELF word          |
        +----------------------------+          +----------------------------+
        |          Value 1           |          |          Word 1            |
        +----------------------------+          +----------------------------+
        |          Value 2           |          |          Word 2            |
        +----------------------------+          +----------------------------+
        |          Value ...         |          |          Word ...          |
        +----------------------------+          +----------------------------+

        The word list holds word datatype values of the structure:

                Type:   word, 'word, :word, word:, /word
                Symbol: actual symbol
                Canon:  canonical symbol
                Typeset: index of the value's typeset, or zero

        This list is used for binding, evaluation, type checking, and
        can also be used for molding.

        When a frame is cloned, only the value block itself need be
        created. The word list remains the same. For functions, the
        value block can be pushed on the stack.

        Frame creation patterns:

            1. Function specification to frame. Spec is scanned for
            words and datatypes, from which the word list is created.
            Closures are identical.

            2. Object specification to frame. Spec is scanned for
            word definitions and merged with parent defintions. An
            option is to allow the words to be typed.

            3. Module words to frame. They are not normally known in
            advance, they are collected during the global binding of a
            newly loaded block. This requires either preallocation of
            the module frame, or some kind of special scan to track
            the new words.

            4. Special frames, such as system natives and actions
            may be created by specific block scans and appending to
            a given frame.
*/

#include "sys-core.h"


//
//  Alloc_Context: C
// 
// Create context of a given size, allocating space for both words and values.
//
// This context will not have its ANY-OBJECT! REBVAL in the [0] position fully
// configured, hence this is an "Alloc" instead of a "Make" (because there
// is still work to be done before it will pass ASSERT_CONTEXT).
//
REBCTX *Alloc_Context(REBCNT len)
{
    REBARR *varlist = Make_Array(len + 1); // size + room for ROOTVAR
    SET_ARR_FLAG(varlist, ARRAY_FLAG_CONTEXT_VARLIST);

    // varlist[0] is a value instance of the OBJECT!/MODULE!/PORT!/ERROR! we
    // are building which contains this context.

    REBVAL *rootvar = Alloc_Tail_Array(varlist);
    SET_TRASH_IF_DEBUG(rootvar);
    rootvar->payload.any_context.varlist = varlist;

    // keylist[0] is the "rootkey" which we currently initialize to SYM_0

    REBARR *keylist = Make_Array(len + 1); // size + room for ROOTKEY
    Val_Init_Typeset(Alloc_Tail_Array(keylist), ALL_64, SYM_0);
    ARR_SERIES(keylist)->misc.meta = NULL; // GC sees meta object, must init

    // varlists link keylists via REBSER.misc field, sharable hence managed

    INIT_CTX_KEYLIST_UNIQUE(AS_CONTEXT(varlist), keylist);
    MANAGE_ARRAY(keylist);

    return AS_CONTEXT(varlist); // varlist pointer is context handle
}


//
//  Expand_Context_Keylist_Core: C
//
// Returns whether or not the expansion invalidated existing keys.
//
REBOOL Expand_Context_Keylist_Core(REBCTX *context, REBCNT delta)
{
    if (delta == 0) return FALSE;

    REBARR *keylist = CTX_KEYLIST(context);
    if (GET_ARR_FLAG(keylist, KEYLIST_FLAG_SHARED)) {
        //
        // INIT_CTX_KEYLIST_SHARED was used to set the flag that indicates
        // this keylist is shared with one or more other contexts.  Can't
        // expand the shared copy without impacting the others, so break away
        // from the sharing group by making a new copy.
        //
        // (If all shared copies break away in this fashion, then the last
        // copy of the dangling keylist will be GC'd.)
        //
        // Keylists are only typesets, so no need for a specifier.

        REBCTX *meta = ARR_SERIES(keylist)->misc.meta; // preserve meta object

        keylist = Copy_Array_Extra_Shallow(keylist, SPECIFIED, delta);        

        ARR_SERIES(keylist)->misc.meta = meta;

        MANAGE_ARRAY(keylist);
        INIT_CTX_KEYLIST_UNIQUE(context, keylist);

        return TRUE;
    }

    // INIT_CTX_KEYLIST_UNIQUE was used to set this keylist in the
    // context, and no INIT_CTX_KEYLIST_SHARED was used by another context
    // to mark the flag indicating it's shared.  Extend it directly.

    Extend_Series(ARR_SERIES(keylist), delta);
    TERM_ARRAY(keylist);

    return FALSE;
}


//
//  Ensure_Keylist_Unique_Invalidated: C
//
// Returns true if the keylist had to be changed to make it unique.
//
REBOOL Ensure_Keylist_Unique_Invalidated(REBCTX *context)
{
    return Expand_Context_Keylist_Core(context, 0);
}


//
//  Expand_Context: C
//
// Expand a context. Copy words if keylist is not unique.
//
void Expand_Context(REBCTX *context, REBCNT delta)
{
    // varlist is unique to each object--expand without making a copy.
    //
    Extend_Series(ARR_SERIES(CTX_VARLIST(context)), delta);
    TERM_ARRAY(CTX_VARLIST(context));

    Expand_Context_Keylist_Core(context, delta);
}


//
//  Append_Context_Core: C
// 
// Append a word to the context word list. Expands the list if necessary.
// Returns the value cell for the word.  The new variable is unset by default.
//
// !!! Review if it would make more sense to use TRASH.
//
// If word is not NULL, use the word sym and bind the word value, otherwise
// use sym.  When using a word, it will be modified to be specifically bound
// to this context after the operation.
//
// !!! Should there be a clearer hint in the interface, with a REBVAL* out,
// to give a fully bound value as a result?  Given that the caller passed
// in the context and can get the index out of a relatively bound word,
// they usually likely don't need the result directly.
//
REBVAL *Append_Context_Core(
    REBCTX *context,
    RELVAL *word,
    REBSYM sym,
    REBOOL lookahead
) {
    REBARR *keylist = CTX_KEYLIST(context);
    RELVAL *value;

    // Add the key to key list
    //
    EXPAND_SERIES_TAIL(ARR_SERIES(keylist), 1);
    value = ARR_LAST(keylist);
    Val_Init_Typeset(value, ALL_64, word ? VAL_WORD_SYM(word) : sym);
    TERM_ARRAY(keylist);

    if (lookahead)
        SET_VAL_FLAG(value, TYPESET_FLAG_LOOKBACK);

    // Add an unset value to var list
    //
    EXPAND_SERIES_TAIL(ARR_SERIES(CTX_VARLIST(context)), 1);
    value = ARR_LAST(CTX_VARLIST(context));
    SET_VOID(value);
    TERM_ARRAY(CTX_VARLIST(context));

    if (word) {
        REBCNT len = CTX_LEN(context);

        // We want to not just add a key/value pairing to the context, but we
        // want to bind a word while we are at it.  Make sure symbol is valid.
        //
        assert(sym == SYM_0);

        // When a binding is made to an ordinary context, the value list is
        // used as the target and the index is a positive number.  Note that
        // for stack-relative bindings, the index will be negative and the
        // target will be a function's PARAMLIST series.
        //
        assert(!GET_VAL_FLAG(word, VALUE_FLAG_RELATIVE));
        SET_VAL_FLAG(word, WORD_FLAG_BOUND);
        INIT_WORD_CONTEXT(word, context);
        INIT_WORD_INDEX(word, len); // length we just bumped
    }
    else
        assert(sym != SYM_0);

    // The variable value location for the key we just added.  It's currently
    // unset (maybe trash someday?) but in either case, known to not be
    // a relative any-word or any-array
    //
    return KNOWN(value);
}


//
//  Append_Context: C
//
// Most common appending is not concerned with lookahead bit (e.g. whether the
// key is infix).  Generally only an issue when copying.
//
REBVAL *Append_Context(REBCTX *context, RELVAL *word, REBSYM sym) {
    return Append_Context_Core(context, word, sym, FALSE);
}


//
//  Copy_Context_Shallow_Extra: C
//
// Makes a copy of a context.  If no extra storage space is requested, then
// the same keylist will be used.
//
REBCTX *Copy_Context_Shallow_Extra(REBCTX *src, REBCNT extra) {
    REBCTX *dest;

    assert(GET_ARR_FLAG(CTX_VARLIST(src), ARRAY_FLAG_CONTEXT_VARLIST));
    assert(GET_ARR_FLAG(CTX_KEYLIST(src), SERIES_FLAG_MANAGED));

    REBCTX *meta = CTX_META(src); // preserve meta object (if any)

    // Note that keylists contain only typesets (hence no relative values),
    // and no varlist is part of a function body.  All the values here should
    // be fully specified.
    //
    if (extra == 0) {
        dest = AS_CONTEXT(Copy_Array_Shallow(CTX_VARLIST(src), SPECIFIED));
        INIT_CTX_KEYLIST_SHARED(dest, CTX_KEYLIST(src));
    }
    else {
        REBARR *keylist = Copy_Array_Extra_Shallow(
            CTX_KEYLIST(src), SPECIFIED, extra
        );
        dest = AS_CONTEXT(Copy_Array_Extra_Shallow(
            CTX_VARLIST(src), SPECIFIED, extra
        ));
        INIT_CTX_KEYLIST_UNIQUE(dest, keylist);
        MANAGE_ARRAY(CTX_KEYLIST(dest));
    }

    SET_ARR_FLAG(CTX_VARLIST(dest), ARRAY_FLAG_CONTEXT_VARLIST);

    CTX_VALUE(dest)->payload.any_context.varlist = CTX_VARLIST(dest);

    INIT_CONTEXT_META(dest, meta); // will be placed on new keylist

    return dest;
}


//
//  Copy_Context_Shallow: C
//
// !!! Make this a macro when there's a place to put it.
//
REBCTX *Copy_Context_Shallow(REBCTX *src) {
    return Copy_Context_Shallow_Extra(src, 0);
}


//
//  Collect_Keys_Start: C
// 
// Use the Bind_Table to start collecting new keys for a context.
// Use Collect_Keys_End() when done.
// 
// WARNING: This routine uses the shared BUF_COLLECT rather than
// targeting a new series directly.  This way a context can be
// allocated at exactly the right length when contents are copied.
// Therefore do not call code that might call BIND or otherwise
// make use of the Bind_Table or BUF_COLLECT.
//
void Collect_Keys_Start(REBFLGS flags)
{
    ASSERT_BIND_TABLE_EMPTY;

    assert(ARR_LEN(BUF_COLLECT) == 0); // should be empty

    // Add a key to slot zero.  When the keys are copied out to be the
    // keylist for a context it will be the CTX_ROOTKEY in the [0] slot.
    //
    Val_Init_Typeset(ARR_HEAD(BUF_COLLECT), ALL_64, SYM_0);

    SET_ARRAY_LEN(BUF_COLLECT, 1);
}


//
//  Grab_Collected_Keylist_Managed: C
//
// The BUF_COLLECT is used to gather keys, which may wind up not requiring any
// new keys from the `prior` that was passed in.  If this is the case, then
// that prior keylist is returned...otherwise a new one is created.
//
// !!! "Grab" is used because "Copy_Or_Reuse" is long, and is picked to draw
// attention to look at the meaning.  Better short communicative name?
//
REBARR *Grab_Collected_Keylist_Managed(REBCTX *prior)
{
    REBARR *keylist;

    // We didn't terminate as we were collecting, so terminate now.
    //
    assert(ARR_LEN(BUF_COLLECT) >= 1); // always at least [0] for rootkey
    TERM_ARRAY(BUF_COLLECT);

#if !defined(NDEBUG)
    //
    // When the key collecting is done, we may be asked to give back a keylist
    // and when we do, if nothing was added beyond the `prior` then that will
    // be handed back.  The array handed back will always be managed, so if
    // we create it then it will be, and if we reuse the prior it will be.
    //
    if (prior) ASSERT_ARRAY_MANAGED(CTX_KEYLIST(prior));
#endif

    // If no new words, prior context.  Note length must include the slot
    // for the rootkey...and note also this means the rootkey cell *may*
    // be shared between all keylists when you pass in a prior.
    //
    if (prior && ARR_LEN(BUF_COLLECT) == CTX_LEN(prior) + 1) {
        keylist = CTX_KEYLIST(prior);
    }
    else {
        // The BUF_COLLECT should contain only typesets, so no relative values
        //
        keylist = Copy_Array_Shallow(BUF_COLLECT, SPECIFIED);
        MANAGE_ARRAY(keylist);
    }

    ARR_SERIES(keylist)->misc.meta = NULL; // clear meta object (GC sees this)

    return keylist;
}


//
//  Collect_Keys_End: C
//
// Free the Bind_Table for reuse and empty the BUF_COLLECT.
//
void Collect_Keys_End(void)
{
    RELVAL *key;
    REBINT *binds = WORDS_HEAD(Bind_Table);

    // We didn't terminate as we were collecting, so terminate now.
    //
    assert(ARR_LEN(BUF_COLLECT) >= 1); // always at least [0] for rootkey
    TERM_ARRAY(BUF_COLLECT);

    // Reset binding table (note BUF_COLLECT may have expanded)
    //
    for (key = ARR_HEAD(BUF_COLLECT); NOT_END(key); key++) {
        assert(IS_TYPESET(key));
        binds[VAL_TYPESET_CANON(key)] = 0;
    }

    SET_ARRAY_LEN(BUF_COLLECT, 0); // allow reuse

    ASSERT_BIND_TABLE_EMPTY;
}


//
//  Collect_Context_Keys: C
// 
// Collect words from a prior context.  If `check_dups` is passed in then
// there is a check for duplicates, otherwise the keys are assumed to
// be unique and copied in using `memcpy` as an optimization.
//
void Collect_Context_Keys(REBCTX *context, REBOOL check_dups)
{
    REBVAL *key = CTX_KEYS_HEAD(context);
    REBINT *binds = WORDS_HEAD(Bind_Table);
    REBINT bind_index = ARR_LEN(BUF_COLLECT);
    RELVAL *collect; // can't set until after potential expansion...

    // The BUF_COLLECT buffer should at least have the SYM_0 in its first slot
    // to use as a "rootkey" in the generated keylist (and also that the first
    // binding index we give out is at least 1, since 0 is used in the
    // Bind_Table to mean "word not collected yet").
    //
    assert(bind_index >= 1);

    // this is necessary for memcpy below to not overwrite memory BUF_COLLECT
    // does not own.  (It may make the buffer capacity bigger than necessary
    // if duplicates are found, but the actual buffer length will be set
    // correctly by the end.)
    //
    EXPAND_SERIES_TAIL(ARR_SERIES(BUF_COLLECT), CTX_LEN(context));

    // EXPAND_SERIES_TAIL will increase the ARR_LEN, even though we intend
    // to overwrite it with a possibly shorter length.  Put the length back
    // and now that the expansion is done, get the pointer to where we want
    // to start collecting new typesets.
    //
    SET_SERIES_LEN(ARR_SERIES(BUF_COLLECT), bind_index);
    collect = ARR_TAIL(BUF_COLLECT);

    if (check_dups) {
        // We're adding onto the end of the collect buffer and need to
        // check for duplicates of what's already there.
        //
        for (; NOT_END(key); key++) {
            REBCNT canon = VAL_TYPESET_CANON(key);

            if (binds[canon] != 0) {
                //
                // If we found the typeset's symbol in the bind table already
                // then don't collect it in the buffer again.
                //
                continue;
            }

            // !!! At the moment objects do not heed the typesets in the
            // keys.  If they did, what sort of rule should the typesets
            // have when being inherited?
            //
            *collect = *key;
            ++collect;

            binds[canon] = bind_index++;
        }

        // Increase the length of BUF_COLLLECT by how far `collect` advanced
        // (would be 0 if all the keys were duplicates...)
        //
        SET_ARRAY_LEN(
            BUF_COLLECT,
            ARR_LEN(BUF_COLLECT) + (collect - ARR_TAIL(BUF_COLLECT))
        );
    }
    else {
        // Optimized copy of the keys.  We can use `memcpy` because these are
        // typesets that are just 64-bit bitsets plus a symbol ID; there is
        // no need to clone the REBVALs to give the copies new identity.
        //
        // Add the keys and bump the length of the collect buffer after
        // (prior to that, the tail should be on the END marker of
        // the existing content--if any)
        //
        memcpy(collect, key, CTX_LEN(context) * sizeof(REBVAL));
        SET_ARRAY_LEN(
            BUF_COLLECT, ARR_LEN(BUF_COLLECT) + CTX_LEN(context)
        );

        for (; NOT_END(key); key++) {
            REBCNT canon = VAL_TYPESET_CANON(key);
            binds[canon] = bind_index++;
        }
    }

    // BUF_COLLECT doesn't get terminated as its being built, but it gets
    // terminated in Collect_Keys_End()
}


//
//  Collect_Context_Inner_Loop: C
// 
// The inner recursive loop used for Collect_Context function below.
//
static void Collect_Context_Inner_Loop(
    REBINT *binds,
    const RELVAL *head,
    REBFLGS flags
) {
    const RELVAL *value = head;
    for (; NOT_END(value); value++) {
        if (ANY_WORD(value)) {
            if (!binds[VAL_WORD_CANON(value)]) {  // only once per word
                if (IS_SET_WORD(value) || (flags & COLLECT_ANY_WORD)) {
                    REBVAL *typeset;
                    binds[VAL_WORD_CANON(value)] = ARR_LEN(BUF_COLLECT);
                    EXPAND_SERIES_TAIL(ARR_SERIES(BUF_COLLECT), 1);
                    typeset = KNOWN(ARR_LAST(BUF_COLLECT));
                    Val_Init_Typeset(
                        typeset,
                        // Allow all datatypes but no void (initially):
                        ~FLAGIT_KIND(REB_0),
                        VAL_WORD_SYM(value)
                    );
                }
            } else {
                // If word duplicated:
                if (flags & COLLECT_NO_DUP) {
                    // Reset binding table (note BUF_COLLECT may have expanded):
                    RELVAL *key = ARR_HEAD(BUF_COLLECT);
                    for (; NOT_END(key); key++)
                        binds[VAL_TYPESET_CANON(key)] = 0;
                    SET_ARRAY_LEN(BUF_COLLECT, 0);  // allow reuse
                    fail (Error(RE_DUP_VARS, value));
                }
            }
            continue;
        }
        // Recurse into sub-blocks:
        if (ANY_EVAL_BLOCK(value) && (flags & COLLECT_DEEP))
            Collect_Context_Inner_Loop(binds, VAL_ARRAY_AT(value), flags);
    }
}


//
//  Collect_Keylist_Managed: C
//
// Scans a block for words to extract and make into typeset keys to go in
// a context.  The Bind_Table is used to quickly determine duplicate entries.
//
// A `prior` context can be provided to serve as a basis; all the keys in
// the prior will be returned, with only new entries contributed by the
// data coming from the head[] array.  If no new values are needed (the
// array has no relevant words, or all were just duplicates of words already
// in prior) then then `prior`'s keylist may be returned.  The result is
// always pre-managed, because it may not be legal to free prior's keylist.
//
// Returns:
//     A block of typesets that can be used for a context keylist.
//     If no new words, the prior list is returned.
//
// !!! There was previously an optimization in object creation which bypassed
// key collection in the case where head[] was empty.  Revisit if it is worth
// the complexity to move handling for that case in this routine.
//
REBARR *Collect_Keylist_Managed(
    REBCNT *self_index_out, // which context index SELF is in (if COLLECT_SELF)
    const RELVAL *head,
    REBCTX *prior,
    REBFLGS flags // see %sys-core.h for COLLECT_ANY_WORD, etc.
) {
    REBINT *binds = WORDS_HEAD(Bind_Table);
    REBARR *keylist;

    Collect_Keys_Start(flags);

    if (flags & COLLECT_ENSURE_SELF) {
        if (
            !prior
            || (
                (*self_index_out = Find_Word_In_Context(prior, SYM_SELF, TRUE))
                == 0
            )
        ) {
            // No prior or no SELF in prior, so we'll add it as the first key
            //
            RELVAL *self_key = ARR_AT(BUF_COLLECT, 1);
            Val_Init_Typeset(self_key, ALL_64, SYM_SELF);

            // !!! See notes on the flags about why SELF is set hidden but
            // not unbindable with TYPESET_FLAG_UNBINDABLE.
            //
            SET_VAL_FLAG(self_key, TYPESET_FLAG_HIDDEN);

            binds[VAL_TYPESET_CANON(self_key)] = 1;
            *self_index_out = 1;
            SET_ARRAY_LEN(BUF_COLLECT, 2); // TASK_BUF_COLLECT is at least 2
        }
        else {
            // No need to add SELF if it's going to be added via the `prior`
            // so just return the `self_index_out` as-is.
        }
    }
    else {
        assert(self_index_out == NULL);
    }

    // Setup binding table with existing words, no need to check duplicates
    //
    if (prior) Collect_Context_Keys(prior, FALSE);

    // Scan for words, adding them to BUF_COLLECT and bind table:
    Collect_Context_Inner_Loop(WORDS_HEAD(Bind_Table), head, flags);

    keylist = Grab_Collected_Keylist_Managed(prior);

    Collect_Keys_End();

    return keylist;
}


//
//  Collect_Words_Inner_Loop: C
// 
// Used for Collect_Words() after the binds table has
// been set up.
//
static void Collect_Words_Inner_Loop(
    REBINT binds[],
    const RELVAL *head,
    REBFLGS flags
) {
    const RELVAL *value = head;
    for (; NOT_END(value); value++) {
        if (ANY_WORD(value)
            && !binds[VAL_WORD_CANON(value)]
            && (IS_SET_WORD(value) || (flags & COLLECT_ANY_WORD))
        ) {
            REBVAL *word;
            binds[VAL_WORD_CANON(value)] = 1;
            word = Alloc_Tail_Array(BUF_COLLECT);
            Val_Init_Word(word, REB_WORD, VAL_WORD_SYM(value));
        }
        else if (ANY_EVAL_BLOCK(value) && (flags & COLLECT_DEEP))
            Collect_Words_Inner_Loop(binds, VAL_ARRAY_AT(value), flags);
    }
}


//
//  Collect_Words: C
// 
// Collect words from a prior block and new block.
//
REBARR *Collect_Words(
    const RELVAL *head,
    RELVAL *opt_prior_head,
    REBFLGS flags
) {
    REBARR *array;
    REBCNT start;
    REBINT *binds = WORDS_HEAD(Bind_Table); // GC safe to do here
    ASSERT_BIND_TABLE_EMPTY;

    assert(ARR_LEN(BUF_COLLECT) == 0); // should be empty

    if (opt_prior_head)
        Collect_Words_Inner_Loop(binds, opt_prior_head, COLLECT_ANY_WORD);

    start = ARR_LEN(BUF_COLLECT);
    Collect_Words_Inner_Loop(binds, head, flags);
    TERM_ARRAY(BUF_COLLECT);

    // Reset word markers:
    {
        RELVAL *word;
        for (word = ARR_HEAD(BUF_COLLECT); NOT_END(word); word++)
            binds[VAL_WORD_CANON(word)] = 0;
    }

    // The words in BUF_COLLECT are newly created, and should not be bound
    // at all... hence fully specified with no relative words
    //
    array = Copy_Array_At_Max_Shallow(
        BUF_COLLECT, start, SPECIFIED, ARR_LEN(BUF_COLLECT) - start
    );
    SET_ARRAY_LEN(BUF_COLLECT, 0);  // allow reuse

    ASSERT_BIND_TABLE_EMPTY;
    return array;
}


//
//  Rebind_Context_Deep: C
// 
// Clone old context to new context knowing
// which types of values need to be copied, deep copied, and rebound.
//
void Rebind_Context_Deep(REBCTX *src, REBCTX *dst, REBINT *opt_binds)
{
    Rebind_Values_Deep(src, dst, CTX_VARS_HEAD(dst), opt_binds);
}


//
//  Make_Selfish_Context_Detect: C
//
// Create a context by detecting top-level set-words in an array of values.
// So if the values were the contents of the block `[a: 10 b: 20]` then the
// resulting context would be for two words, `a` and `b`.
//
// Optionally a parent context may be passed in, which will contribute its
// keylist of words to the result if provided.
//
// The resulting context will have a SELF: defined as a hidden key (will not
// show up in `words-of` but will be bound during creation).  As part of
// the migration away from SELF being a keyword, the logic for adding and
// managing SELF has been confined to this function (called by `make object!`
// and some other context-creating routines).  This will ultimately turn
// into something paralleling the non-keyword definitional RETURN:, where
// the generators (like OBJECT) will be taking responsibility for it.
//
// This routine will *always* make a context with a SELF.  This lacks the
// nuance that is expected of the generators, which will have an equivalent
// to <no-return>.
//
REBCTX *Make_Selfish_Context_Detect(
    enum Reb_Kind kind,
    REBCTX *spec,
    REBARR *exit_from,
    const RELVAL *head,
    REBCTX *opt_parent
) {
    REBCNT self_index;
    REBARR *keylist = Collect_Keylist_Managed(
        &self_index,
        head,
        opt_parent,
        COLLECT_ONLY_SET_WORDS | COLLECT_ENSURE_SELF
    );

    REBCNT len = ARR_LEN(keylist);

    // Make a context of same size as keylist (END already accounted for)
    //
    REBARR *varlist = Make_Array(len);
    SET_ARR_FLAG(varlist, ARRAY_FLAG_CONTEXT_VARLIST);

    REBCTX *context = AS_CONTEXT(varlist);

    // !!! We actually don't know if the keylist coming back from
    // Collect_Keylist_Managed was created new or reused.  Err on the safe
    // side for now, but it could also return a result so we could know
    // if it would be legal to call INIT_CTX_KEYLIST_UNIQUE.
    //
    INIT_CTX_KEYLIST_SHARED(context, keylist);

    // context[0] is an instance value of the OBJECT!/PORT!/ERROR!/MODULE!
    //
    CTX_VALUE(context)->payload.any_context.varlist = varlist;
    VAL_CONTEXT_EXIT_FROM(CTX_VALUE(context)) = NULL;

    SET_ARRAY_LEN(CTX_VARLIST(context), len);

    // !!! This code was inlined from Create_Frame() because it was only
    // used once here, and it filled the context vars with NONE!.  For
    // Ren-C we probably want to go with void, and also the filling
    // of parent vars will overwrite the work here.  Review.
    //
    {
        REBVAL *var = CTX_VARS_HEAD(context);
        for (; len > 1; len--, var++) // 1 is rootvar (context), already done
            SET_BLANK(var);
        SET_END(var);
    }

    if (opt_parent) {
        //
        // Bitwise copy parent values (will have bits fixed by Clonify).
        // None of these should be relative, because they came from object
        // vars (that were not part of the deep copy of a function body)
        //
        memcpy(
            CTX_VARS_HEAD(context),
            CTX_VARS_HEAD(opt_parent),
            (CTX_LEN(opt_parent)) * sizeof(REBVAL)
        );

        // For values we copied that were blocks and strings, replace
        // their series components with deep copies of themselves:
        //
        Clonify_Values_Len_Managed(
            CTX_VARS_HEAD(context),
            SPECIFIED,
            CTX_LEN(context),
            TRUE,
            TS_CLONE
        );
    }

    VAL_RESET_HEADER(CTX_VALUE(context), kind);
    assert(CTX_TYPE(context) == kind);

    INIT_CONTEXT_META(context, spec);
    VAL_CONTEXT_EXIT_FROM(CTX_VALUE(context)) = exit_from;

    // We should have a SELF key in all cases here.  Set it to be a copy of
    // the object we just created.  (It is indeed a copy of the [0] element,
    // but it doesn't need to be protected because the user overwriting it
    // won't destroy the integrity of the context.)
    //
    assert(CTX_KEY_CANON(context, self_index) == SYM_SELF);
    *CTX_VAR(context, self_index) = *CTX_VALUE(context);

    // !!! In Ren-C, the idea that functions are rebound when a context is
    // inherited is being deprecated.  It simply isn't viable for objects
    // with N methods to have those N methods permanently cloned in the
    // copies and have their bodies rebound to the new object.  A more
    // conventional method of `this->method()` access is needed with
    // cooperation from the evaluator, and that is slated to be `/method`
    // as a practical use of paths that implicitly start from "wherever
    // you dispatched from"
    //
    // Temporarily the old behavior is kept, so we deep copy and rebind.
    //
    if (opt_parent)
        Rebind_Context_Deep(opt_parent, context, NULL);

    ASSERT_CONTEXT(context);

#if !defined(NDEBUG)
    PG_Reb_Stats->Objects++;
#endif

    return context;
}


//
//  Construct_Context: C
// 
// Construct an object without evaluation.
// Parent can be null. Values are rebound.
//
// In R3-Alpha the CONSTRUCT native supported a mode where the following:
//
//      [a: b: 1 + 2 d: a e:]
//
// ...would have `a` and `b` will be set to 1, while `+` and `2` will be
// ignored, `d` will be the word `a` (where it knows to be bound to the a
// of the object) and `e` would be left as it was.
//
// Ren-C retakes the name CONSTRUCT to be the arity-2 object creation
// function with evaluation, and makes "raw" construction (via /ONLY on both
// 1-arity HAS and CONSTRUCT) more regimented.  The requirement for a raw
// construct is that the fields alternate SET-WORD! and then value, with
// no evaluation--hence it is possible to use any value type (a GROUP! or
// another SET-WORD!, for instance) as the value.
//
// !!! Because this is a work in progress, set-words would be gathered if
// they were used as values, so they are not currently permitted.
//
REBCTX *Construct_Context(
    enum Reb_Kind kind,
    RELVAL *head, // !!! Warning: modified binding
    REBCTX *specifier,
    REBCTX *opt_parent
) {
    REBCTX *context = Make_Selfish_Context_Detect(
        kind, // type
        NULL, // spec
        NULL, // body
        head, // values to scan for toplevel set-words
        opt_parent // parent
    );

    const RELVAL *value = head ? head : END_CELL;

    if (head) Bind_Values_Shallow(head, context);

    for (; NOT_END(value); value += 2) {
        //
        // !!! Objects are a rewrite in progress; error messages need to
        // be improved.

        REBVAL *var;

        if (!IS_SET_WORD(value))
            fail (Error(RE_INVALID_TYPE, Type_Of(value)));

        if (IS_END(value + 1))
            fail (Error(RE_MISC));

        assert(!IS_SET_WORD(value + 1)); // TBD: support set words!

        var = GET_MUTABLE_VAR_MAY_FAIL(value, specifier);

        COPY_VALUE(var, value + 1, specifier);
    }

    return context;
}


//
//  Context_To_Array: C
// 
// Return a block containing words, values, or set-word: value
// pairs for the given object. Note: words are bound to original
// object.
// 
// Modes:
//     1 for word
//     2 for value
//     3 for words and values
//
REBARR *Context_To_Array(REBCTX *context, REBINT mode)
{
    REBVAL *key = CTX_KEYS_HEAD(context);
    REBVAL *var = CTX_VARS_HEAD(context);
    REBARR *block;
    REBVAL *value;
    REBCNT n;

    assert(!(mode & 4));
    block = Make_Array(CTX_LEN(context) * (mode == 3 ? 2 : 1));

    n = 1;
    for (; !IS_END(key); n++, key++, var++) {
        if (!GET_VAL_FLAG(key, TYPESET_FLAG_HIDDEN)) {
            if (mode & 1) {
                value = Alloc_Tail_Array(block);
                if (mode & 2) {
                    VAL_RESET_HEADER(value, REB_SET_WORD);
                    SET_VAL_FLAG(value, VALUE_FLAG_LINE);
                }
                else VAL_RESET_HEADER(value, REB_WORD);

                INIT_WORD_SYM(value, VAL_TYPESET_SYM(key));
                SET_VAL_FLAG(value, WORD_FLAG_BOUND); // hdr reset, !relative
                INIT_WORD_CONTEXT(value, context);
                INIT_WORD_INDEX(value, n);
            }
            if (mode & 2) {
                Append_Value(block, var);
            }
        }
    }

    return block;
}


//
//  Merge_Contexts_Selfish: C
// 
// Create a child context from two parent contexts. Merge common fields.
// Values from the second parent take precedence.
// 
// Deep copy and rebind the child.
//
REBCTX *Merge_Contexts_Selfish(REBCTX *parent1, REBCTX *parent2)
{
    REBARR *keylist;
    REBCTX *child;
    REBVAL *key;
    REBVAL *value;
    REBCNT n;
    REBINT *binds = WORDS_HEAD(Bind_Table);

    assert(CTX_TYPE(parent1) == CTX_TYPE(parent2));

    // Merge parent1 and parent2 words.
    // Keep the binding table.
    Collect_Keys_Start(COLLECT_ANY_WORD | COLLECT_ENSURE_SELF);

    // Setup binding table and BUF_COLLECT with parent1 words.  Don't bother
    // checking for duplicates, buffer is empty.
    //
    Collect_Context_Keys(parent1, FALSE);

    // Add parent2 words to binding table and BUF_COLLECT, and since we know
    // BUF_COLLECT isn't empty then *do* check for duplicates.
    //
    Collect_Context_Keys(parent2, TRUE);

    // Collect_Keys_End() terminates, but Collect_Context_Inner_Loop() doesn't.
    //
    TERM_ARRAY(BUF_COLLECT);

    // Allocate child (now that we know the correct size).  Obey invariant
    // that keylists are always managed.  The BUF_COLLECT contains only
    // typesets, so no need for a specifier in the copy.
    //
    // !!! Review: should child start fresh with no meta information, or get
    // the meta information held by parents?
    //
    keylist = Copy_Array_Shallow(BUF_COLLECT, SPECIFIED);
    MANAGE_ARRAY(keylist);
    ARR_SERIES(keylist)->misc.meta = NULL;

    child = AS_CONTEXT(Make_Array(ARR_LEN(keylist)));
    SET_ARR_FLAG(CTX_VARLIST(child), ARRAY_FLAG_CONTEXT_VARLIST);

    value = Alloc_Tail_Array(CTX_VARLIST(child));

    // !!! Currently we assume the child will be of the same type as the
    // parent...so if the parent was an OBJECT! so will the child be, if
    // the parent was an ERROR! so will the child be.  This is a new idea,
    // so review consequences.
    //
    VAL_RESET_HEADER(value, CTX_TYPE(parent1));
    INIT_CTX_KEYLIST_UNIQUE(child, keylist);
    value->payload.any_context.varlist = CTX_VARLIST(child);
    VAL_CONTEXT_EXIT_FROM(value) = NULL;

    // Copy parent1 values:
    memcpy(
        CTX_VARS_HEAD(child),
        CTX_VARS_HEAD(parent1),
        CTX_LEN(parent1) * sizeof(REBVAL)
    );

    // Update the child tail before making calls to CTX_VAR(), because the
    // debug build does a length check.
    //
    SET_ARRAY_LEN(CTX_VARLIST(child), ARR_LEN(keylist));

    // Copy parent2 values:
    key = CTX_KEYS_HEAD(parent2);
    value = CTX_VARS_HEAD(parent2);
    for (; NOT_END(key); key++, value++) {
        // no need to search when the binding table is available
        n = binds[VAL_TYPESET_CANON(key)];
        *CTX_VAR(child, n) = *value;
    }

    // Terminate the child context:
    TERM_ARRAY(CTX_VARLIST(child));

    // Deep copy the child.  Context vars are REBVALs, already fully specified
    //
    Clonify_Values_Len_Managed(
        CTX_VARS_HEAD(child),
        SPECIFIED,
        CTX_LEN(child),
        TRUE,
        TS_CLONE
    );

    // Rebind the child
    Rebind_Context_Deep(parent1, child, NULL);
    Rebind_Context_Deep(parent2, child, WORDS_HEAD(Bind_Table));

    // release the bind table
    Collect_Keys_End();

    // We should have gotten a SELF in the results, one way or another.
    {
        REBCNT self_index = Find_Word_In_Context(child, SYM_SELF, TRUE);
        assert(self_index != 0);
        assert(CTX_KEY_CANON(child, self_index) == SYM_SELF);
        *CTX_VAR(child, self_index) = *CTX_VALUE(child);
    }

    return child;
}


//
//  Resolve_Context: C
// 
// Only_words can be a block of words or an index in the target
// (for new words).
//
void Resolve_Context(
    REBCTX *target,
    REBCTX *source,
    REBVAL *only_words,
    REBOOL all,
    REBOOL expand
) {
    REBINT *binds  = WORDS_HEAD(Bind_Table); // GC safe to do here
    REBVAL *key;
    REBVAL *var;
    REBINT n;
    REBINT m;
    REBCNT i = 0;

    ASSERT_BIND_TABLE_EMPTY;

    FAIL_IF_LOCKED_CONTEXT(target);

    if (IS_INTEGER(only_words)) { // Must be: 0 < i <= tail
        i = VAL_INT32(only_words); // never <= 0
        if (i == 0) i = 1;
        if (i > CTX_LEN(target)) return;
    }

    // !!! This function does its own version of resetting the bind table
    // and hence the Collect_Keys_End that would be performed in the case of
    // a `fail (Error(...))` will not properly reset it.  Because the code
    // does array expansion it cannot guarantee a fail won't happen, hence
    // the method needs to be reviewed to something that could properly
    // reset in the case of an out of memory error.
    //
    Collect_Keys_Start(COLLECT_ONLY_SET_WORDS);

    n = 0;

    // If limited resolve, tag the word ids that need to be copied:
    if (i != 0) {
        // Only the new words of the target:
        for (key = CTX_KEY(target, i); NOT_END(key); key++)
            binds[VAL_TYPESET_CANON(key)] = -1;
        n = CTX_LEN(target);
    }
    else if (IS_BLOCK(only_words)) {
        // Limit exports to only these words:
        RELVAL *word = VAL_ARRAY_AT(only_words);
        for (; NOT_END(word); word++) {
            if (IS_WORD(word) || IS_SET_WORD(word)) {
                binds[VAL_WORD_CANON(word)] = -1;
                n++;
            }
            else {
                // !!! There was no error here.  :-/  Should it be one?
            }
        }
    }

    // Expand target as needed:
    if (expand && n > 0) {
        // Determine how many new words to add:
        for (key = CTX_KEYS_HEAD(target); NOT_END(key); key++)
            if (binds[VAL_TYPESET_CANON(key)]) n--;

        // Expand context by the amount required:
        if (n > 0) Expand_Context(target, n);
        else expand = FALSE;
    }

    // Maps a word to its value index in the source context.
    // Done by marking all source words (in bind table):
    key = CTX_KEYS_HEAD(source);
    for (n = 1; NOT_END(key); n++, key++) {
        if (IS_VOID(only_words) || binds[VAL_TYPESET_CANON(key)])
            binds[VAL_TYPESET_CANON(key)] = n;
    }

    // Foreach word in target, copy the correct value from source:
    //
    var = i != 0 ? CTX_VAR(target, i) : CTX_VARS_HEAD(target);
    key = i != 0 ? CTX_KEY(target, i) : CTX_KEYS_HEAD(target);
    for (; NOT_END(key); key++, var++) {
        if ((m = binds[VAL_TYPESET_CANON(key)])) {
            binds[VAL_TYPESET_CANON(key)] = 0; // mark it as set
            if (
                !GET_VAL_FLAG(key, TYPESET_FLAG_LOCKED)
                && (all || IS_VOID(var))
            ) {
                if (m < 0) SET_VOID(var); // no value in source context
                else {
                    *var = *CTX_VAR(source, m);

                    // Need to also copy if the binding is lookahead (e.g.
                    // would be an infix call).
                    //
                    if (GET_VAL_FLAG(
                        CTX_KEY(source, m), TYPESET_FLAG_LOOKBACK
                    )) {
                        SET_VAL_FLAG(key, TYPESET_FLAG_LOOKBACK);
                    }
                    else
                        CLEAR_VAL_FLAG(key, TYPESET_FLAG_LOOKBACK);
                }
            }
        }
    }

    // Add any new words and values:
    if (expand) {
        key = CTX_KEYS_HEAD(source);
        for (n = 1; NOT_END(key); n++, key++) {
            if (binds[VAL_TYPESET_CANON(key)]) {
                // Note: no protect check is needed here
                binds[VAL_TYPESET_CANON(key)] = 0;
                var = Append_Context_Core(
                    target,
                    0,
                    VAL_TYPESET_CANON(key),
                    GET_VAL_FLAG(key, TYPESET_FLAG_LOOKBACK)
                );
                *var = *CTX_VAR(source, n);
            }
        }
    }
    else {
        // Reset bind table (do not use Collect_End):
        if (i != 0) {
            for (key = CTX_KEY(target, i); NOT_END(key); key++)
                binds[VAL_TYPESET_CANON(key)] = 0;
        }
        else if (IS_BLOCK(only_words)) {
            RELVAL *word = VAL_ARRAY_AT(only_words);
            for (; NOT_END(word); word++) {
                if (IS_WORD(word) || IS_SET_WORD(word))
                    binds[VAL_WORD_CANON(word)] = 0;
                else {
                    // !!! There was no error here.  Should there be?  :-/
                }
            }
        }
        else {
            for (key = CTX_KEYS_HEAD(source); NOT_END(key); key++)
                binds[VAL_TYPESET_CANON(key)] = 0;
        }
    }

    ASSERT_BIND_TABLE_EMPTY;

    // !!! Note we explicitly do *not* use Collect_Keys_End().  See warning
    // about errors, out of memory issues, etc. at Collect_Keys_Start()
    //
    SET_ARRAY_LEN(BUF_COLLECT, 0);  // allow reuse
}


//
//  Find_Word_In_Context: C
// 
// Search a context looking for the given word symbol.
// Return the context index for a word. Locate it by matching
// the canon word identifiers. Return 0 if not found.
//
REBCNT Find_Word_In_Context(REBCTX *context, REBSYM sym, REBOOL always)
{
    REBVAL *key = CTX_KEYS_HEAD(context);
    REBCNT len = CTX_LEN(context);

    REBCNT canon = SYMBOL_TO_CANON(sym); // always compare to CANON sym

    REBCNT n;
    for (n = 1; n <= len; n++, key++) {
        if (
            sym == VAL_TYPESET_SYM(key)
            || canon == VAL_TYPESET_CANON(key)
        ) {
            return (!always && GET_VAL_FLAG(key, TYPESET_FLAG_HIDDEN)) ? 0 : n;
        }
    }

    // !!! Should this be changed to NOT_FOUND?
    return 0;
}


//
//  Find_Word_Value: C
// 
// Search a frame looking for the given word symbol and
// return the value for the word. Locate it by matching
// the canon word identifiers. Return NULL if not found.
//
REBVAL *Find_Word_Value(REBCTX *context, REBSYM sym)
{
    REBINT n;

    if (!context) return 0;
    n = Find_Word_In_Context(context, sym, FALSE);
    if (n == 0) return 0;
    return CTX_VAR(context, n);
}


//
//  Find_Word_In_Array: C
// 
// Find word (of any type) in an array of values with linear search.
//
REBCNT Find_Word_In_Array(REBARR *array, REBCNT index, REBSYM sym)
{
    RELVAL *value;

    for (; index < ARR_LEN(array); index++) {
        value = ARR_AT(array, index);
        if (ANY_WORD(value) && sym == VAL_WORD_CANON(value))
            return index;
    }

    return NOT_FOUND;
}


//
//  Obj_Value: C
// 
// Return pointer to the nth VALUE of an object.
// Return zero if the index is not valid.
//
REBVAL *Obj_Value(REBVAL *value, REBCNT index)
{
    REBCTX *context = VAL_CONTEXT(value);

    if (index > CTX_LEN(context)) return 0;
    return CTX_VAR(context, index);
}


//
//  Init_Collector: C
//
void Init_Collector(void)
{
    // Temporary block used while scanning for frame words:
    // "just holds typesets, no GC behavior" (!!! until typeset symbols or
    // embedded tyeps are GC'd...!)
    //
    // Note that the logic inside Collect_Keylist managed assumes it's at
    // least 2 long to hold the rootkey (SYM_0) and a possible SYM_SELF
    // hidden actual key.
    //
    Set_Root_Series(TASK_BUF_COLLECT, ARR_SERIES(Make_Array(2 + 98)));
}


#ifndef NDEBUG

//
//  Assert_Context_Core: C
//
void Assert_Context_Core(REBCTX *context)
{
    REBCNT n;
    REBVAL *key;
    REBVAL *var;
    REBVAL *rootkey;

    REBCNT keys_len;
    REBCNT vars_len;

    if (!GET_ARR_FLAG(CTX_VARLIST(context), ARRAY_FLAG_CONTEXT_VARLIST)) {
        Debug_Fmt("Context varlist doesn't have ARRAY_FLAG_CONTEXT_VARLIST");
        Panic_Context(context);
    }

    if (!ANY_CONTEXT(CTX_VALUE(context))) {
        Debug_Fmt("Element at head of frame is not an ANY_CONTEXT");
        Panic_Context(context);
    }

    if (!CTX_KEYLIST(context)) {
        Debug_Fmt("Null keylist found in frame");
        Panic_Context(context);
    }

    vars_len = ARR_LEN(CTX_VARLIST(context));
    keys_len = ARR_LEN(CTX_KEYLIST(context));

    if (keys_len < 1) {
        Debug_Fmt("Keylist length less than one--cannot hold rootkey");
        Panic_Context(context);
    }

    if (GET_CTX_FLAG(context, CONTEXT_FLAG_STACK)) {
        assert(vars_len == 1);
    }
    else {
        if (keys_len != vars_len) {
            Debug_Fmt("Unequal lengths of key/var series in Assert_Context");
            Panic_Context(context);
        }
    }

    // The 0th key and var are special and can't be accessed with CTX_VAR
    // or CTX_KEY
    //
    rootkey = CTX_ROOTKEY(context);

    if (
        (IS_TYPESET(rootkey) && VAL_TYPESET_SYM(rootkey) == SYM_0)
        || IS_FUNCTION(rootkey)
    ) {
        // It's okay.  Note that in the future the rootkey for ordinary
        // OBJECT!/ERROR!/PORT! etc. may be more interesting than SYM_0
    }
    else {
        Debug_Fmt("Rootkey in context not SYM_0 or FUNCTION!.");
        Panic_Context(context);
    }

    var = CTX_VALUE(context);

    if (!ANY_CONTEXT(var)) {
        Debug_Fmt("First value slot in context not ANY-CONTEXT!");
        Panic_Context(context);
    }

    if (var->payload.any_context.varlist != CTX_VARLIST(context)) {
        Debug_Fmt("Embedded ANY-CONTEXT!'s context doesn't match context");
        Panic_Context(context);
    }

    if (
        GET_CTX_FLAG(context, CONTEXT_FLAG_STACK)
        && !GET_CTX_FLAG(context, SERIES_FLAG_ACCESSIBLE)
    ) {
        // !!! For the moment, don't check inaccessible stack frames any
        // further.  This includes varless reified frames and those reified
        // frames that are no longer on the stack.
        //
        return;
    }

    key = CTX_KEYS_HEAD(context);
    var = CTX_VARS_HEAD(context);
    REBVAL *xxx = var;

    for (n = 1; n < keys_len; n++, var++, key++) {
        if (IS_END(key) || IS_END(var)) {
            Debug_Fmt(
                "** Early %s end at index: %d",
                IS_END(key) ? "key" : "var",
                n
            );
            Panic_Context(context);
        }

        if (!IS_TYPESET(key)) {
            Debug_Fmt("** Non-typeset in context keys: %d\n", VAL_TYPE(key));
            Panic_Context(context);
        }
    }

    if (NOT_END(key) || NOT_END(var)) {
        Debug_Fmt(
            "** Missing %s end at index: %d type: %d",
            NOT_END(key) ? "key" : "var",
            n,
            NOT_END(key) ? VAL_TYPE(key) : VAL_TYPE(var)
        );
        Panic_Context(context);
    }
}
#endif
