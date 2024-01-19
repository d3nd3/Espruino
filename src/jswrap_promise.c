/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2016 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * This file is designed to be parsed during the build process
 *
 * ES6 Promise implementation
 * ----------------------------------------------------------------------------
 */

#include "jsutils.h"

#if ESPR_NO_PROMISES!=1

#include "jswrap_promise.h"
#include "jsparse.h"
#include "jsinteractive.h"
#include "jswrapper.h"

#define JS_PROMISE_THEN_NAME JS_HIDDEN_CHAR_STR"thn"
#define JS_PROMISE_CATCH_NAME JS_HIDDEN_CHAR_STR"cat"
#define JS_PROMISE_REMAINING_NAME JS_HIDDEN_CHAR_STR"left"
#define JS_PROMISE_RESULT_NAME JS_HIDDEN_CHAR_STR"res"
#define JS_PROMISE_RESOLVED_NAME "resolved"


/*JSON{
  "type" : "class",
  "class" : "Promise",
  "typescript": "Promise<T>",
  "ifndef" : "SAVE_ON_FLASH"
}
This is the built-in class for ES6 Promises
*/

void _jswrap_promise_queueresolve(JsVar *promise, JsVar *data);
void _jswrap_promise_queuereject(JsVar *promise, JsVar *data);
void _jswrap_promise_add(JsVar *parent, JsVar * next, JsVar *callback, bool resolve);

bool _jswrap_promise_is_promise(JsVar *promise) {
  JsVar *constr = jspGetConstructor(promise);
  bool isPromise = constr && (void*)constr->varData.native.ptr==(void*)jswrap_promise_constructor;
  jsvUnLock(constr);
  return isPromise;
}

/*
  The resolving promise, has to set its state to resolved.
  And queue all callbacks registered, which each have a unique 'next' promise to in turn resolve
  This creates an asynchronous recursive loop (aka chain).
  It also handles nested promises who resolve another promise.
  Unless no reactions were attached (no calls to then or catch).
*/
//This function is only called if next is set. A reason to resolve more.
void _jswrap_promise_handle_reaction_call(JsVar* promise, JsVar* next, JsVar *result) {
    jsiConsolePrintf("_jswrap_promise_handle_reaction_call()\n");
    //wasError
    JsVar *exception = jspGetException();
    if (exception) {
      _jswrap_promise_queuereject(next, exception);
      jsvUnLock3(exception, result, next);
      return;
    }
    //noError
    if (_jswrap_promise_is_promise(result)) {
      JsVar *fnres = jsvNewNativeFunction((void (*)(void))_jswrap_promise_queueresolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
      JsVar *fnrej = jsvNewNativeFunction((void (*)(void))_jswrap_promise_queuereject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
      if (fnres && fnrej) {
        jsvObjectSetChild(fnres, JSPARSE_FUNCTION_THIS_NAME, next); // bind 'this'
        jsvObjectSetChild(fnrej, JSPARSE_FUNCTION_THIS_NAME, next); // bind 'this'
        // When this 'inner' promise resolves, it will resolve outer.
        _jswrap_promise_add(result, next, fnres, true);
        _jswrap_promise_add(result, next, fnrej, false);
      }
      jsvUnLock2(fnres,fnrej);
    } else {
      //We returned a non-promise.
      //Continue the loop ;)
      _jswrap_promise_queueresolve(next, result);
    }  
    jsvUnLock2(next,result);
}

/*
RESOLVE/REJECT ASYNCRHONOUS CALLBACK - STAGE 2
EXECUTECALLBACKS.

promise - which promise we are resolving/rejecting
data - the value/error we are resolving/rejecting with
fn - the user registered callbacks passed to then/catch

Executes Settled/Rejected handlers, fn, can be array due to multiple .then calls in unchained manner.

If there is exception, it rejects the promise and returns.
else:
Resolves the .then() shell promise accordingly.
*/
/*
  RESOLVE/REJECT ASYNCRHONOUS CALLBACK - STAGE 1
  finds the nearest handler by iterating the chain.

  ----SHOULD RESOLVE-----
  check if already resolved : return
  set already resolved
  resolving self : reject("Unable to resolve self")
  resolve basic value: resolve2...
  handle thenables
  handle recursive resolve


  resolve2:
      if state !== pending:
          throw("already settled");
      state = fulfilled
      runAllonFullfilled, with their corresponding attached new Promise.( supports multiple chains ).

  ----SHOULD REJECT----
  check if already resolved: return
  set already resolved
  if state !== pending:
    throw("already settled")
  state = rejected
  runAllonRejected, with their corresponding attached new Promise. ( supports multiple chains ).

  ---DOES----
  throw error if resolving promise.
  resolve? set resolved.
  do we have a handler?
    find a handler by iterating chain
  if we have handler for THIS promise, ContinueRejectionResolving
  else we have no handlers, were we rejecting? throw the error.

  ContinueRejectionResolving:
    remove handlers and chain from promise
    executes handlers
    if any handler errored, queues reject, and returns (This should queue reject for the new Promise associated, not all same.)
    resolve the new Promise
    handles 1 step recursive Promise resolution.

  ---DOES NOT---
  check if already resolved
  resolving self: reject


  handle thenables
  handle recursive resolve
*/
/*
  The callback is now stored in promise.thn.cb or promise.cat.cb
  Intended to update the resolve state of promise.thn.next or promise.cat.next
*/
void _jswrap_promise_resolve_or_reject(JsVar *promise, JsVar *data, JsVar *resolve) {
  jsiConsolePrintf("_jswrap_promise_resolve_or_reject()\n");
  JsVar *isResolved = jsvFindChildFromString(promise, JS_PROMISE_RESOLVED_NAME);
  if (isResolved){
    jsvUnLock(isResolved);
    return;
  }
  jsvObjectSetChild(promise, JS_PROMISE_RESOLVED_NAME, data);

  const char *eventName = resolve ? JS_PROMISE_THEN_NAME : JS_PROMISE_CATCH_NAME;
  JsVar *reactions = jsvObjectGetChildIfExists(promise, eventName);
  if (reactions) {
    //=============================================================================
    //This will call all callbacks, but they all have different next/target promises for which
    //they resolve...
    //=============================================================================
    // reactions is actually an object or array of objects, of form {cb:,next:}
    if (jsvIsArray(reactions)) {
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, reactions);   
      while (jsvObjectIteratorHasValue(&it)) {
        JsVar *reactObj = jsvObjectIteratorGetValue(&it);
        JsVar * f = jsvObjectGetChildIfExists(reactObj,"cb");
        if (f) {
          JsVar *v = jspExecuteFunction(f, promise, 1, &data);
          JsVar * nextProm = jsvObjectGetChildIfExists(reactObj,"next");
          if (nextProm)
            _jswrap_promise_handle_reaction_call(promise,nextProm,v);
          jsvUnLock(f);
        }
        jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
    } else if (reactions) {
      // Non-array , size==1
      JsVar * f = jsvObjectGetChildIfExists(reactions,"cb");
      if (f) {
        JsVar *v = jspExecuteFunction(f, promise, 1, &data);
        JsVar * nextProm = jsvObjectGetChildIfExists(reactions,"next");
        if (nextProm)
          _jswrap_promise_handle_reaction_call(promise,nextProm,v);
        jsvUnLock(f);
      }
    }
    jsvUnLock(reactions);
  } else if (!resolve) {
    // Unhandled and rejected
    JsVar *previouslyResolved = jsvFindChildFromString(promise, JS_PROMISE_RESOLVED_NAME);
    if (!previouslyResolved) {
      jsExceptionHere(JSET_ERROR, "Unhandled promise rejection: %v", data);
      // If there was an exception with a stack trace, pass it through so we can keep adding stack to it
      JsVar *stack = 0;
      if (jsvIsObject(data) && (stack=jsvObjectGetChildIfExists(data, "stack"))) {
        jsvObjectSetChildAndUnLock(execInfo.hiddenRoot, JSPARSE_STACKTRACE_VAR, stack);
      }
    }
    jsvUnLock(previouslyResolved);
  }
}

/*
  RESOLVE WRAPPER
  find handler, run handler, update shell promise. (p1.chain)

  =================================================================================================
=================================================================================================
  =======================================RESOLVE()=============================================================
  =================================================================================================
  =================================================================================================
*/
void _jswrap_promise_resolve(JsVar *promise, JsVar *data) {
  _jswrap_promise_resolve_or_reject(promise, data, true);
}
/*
  PASSED AS ARGUMENTS TO THE EXECUTOR.
  find handler, run handler, update shell promise. (p1.chain)

*/
void _jswrap_promise_queueresolve(JsVar *promise, JsVar *data) {
  jsiConsolePrintf("_jswrap_promise_queueresolve()\n");
  JsVar *fn = jsvNewNativeFunction((void (*)(void))_jswrap_promise_resolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
  if (!fn) return;
  jsvObjectSetChild(fn, JSPARSE_FUNCTION_THIS_NAME, promise); // bind 'this'
  jsiQueueEvents(promise, fn, &data, 1);
  jsvUnLock(fn);
}
/*
  REJECT WRAPPER
  find handler, run handler, update shell promise. (p1.chain)

    =================================================================================================
=================================================================================================
  =======================================REJECT()=============================================================
  =================================================================================================
  =================================================================================================
*/
void _jswrap_promise_reject(JsVar *promise, JsVar *data) {
  _jswrap_promise_resolve_or_reject(promise, data, false);
}
/*
  PASSED AS ARGUMENTS TO THE EXECUTOR.
  find handler, run handler, update shell promise. (p1.chain)
*/
void _jswrap_promise_queuereject(JsVar *promise, JsVar *data) {
  jsiConsolePrintf("_jswrap_promise_queuereject()\n");
  JsVar *fn = jsvNewNativeFunction((void (*)(void))_jswrap_promise_reject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
  if (!fn) return;
  jsvObjectSetChild(fn, JSPARSE_FUNCTION_THIS_NAME, promise); // bind 'this'
  jsiQueueEvents(promise, fn, &data, 1);
  jsvUnLock(fn);
}

void jswrap_promise_all_resolve(JsVar *promise, JsVar *index, JsVar *data) {
  JsVarInt remaining = jsvGetIntegerAndUnLock(jsvObjectGetChildIfExists(promise, JS_PROMISE_REMAINING_NAME));
  JsVar *arr = jsvObjectGetChildIfExists(promise, JS_PROMISE_RESULT_NAME);
  if (arr) {
    // set the result
    jsvSetArrayItem(arr, jsvGetInteger(index), data);
    // Update remaining list
    remaining--;
    jsvObjectSetChildAndUnLock(promise, JS_PROMISE_REMAINING_NAME, jsvNewFromInteger(remaining));
    if (remaining==0) {
      _jswrap_promise_queueresolve(promise, arr);
    }
    jsvUnLock(arr);
  }
}
void jswrap_promise_all_reject(JsVar *promise, JsVar *data) {
  JsVar *arr = jsvObjectGetChildIfExists(promise, JS_PROMISE_RESULT_NAME);
  if (arr) {
    // if not rejected before
    jsvUnLock(arr);
    jsvObjectRemoveChild(promise, JS_PROMISE_RESULT_NAME);
    _jswrap_promise_queuereject(promise, data);
  }
}

/// Create a new promise
JsVar *jspromise_create() {
  return jspNewObject(0, "Promise");
}

/*
  WRAPPER TO QUEUE RESOLVE
*/
void jspromise_resolve(JsVar *promise, JsVar *data) {
  _jswrap_promise_queueresolve(promise, data);
}

/// Reject the given promise
void jspromise_reject(JsVar *promise, JsVar *data) {
  _jswrap_promise_queuereject(promise, data);
}


/*
    =================================================================================================
=================================================================================================
  =======================================NEW()=============================================================
  =================================================================================================
  =================================================================================================

  ---SHOULD---
  Does executor NOT exist -> throw error ("Executor function required in promise constructor") 
  Is executor NOT a function -> throw error ("Executor must be callable") 

  Execute Executor, rejecting all errors thrown.

  ---DOES NOT---
  Does executor NOT exist -> throw error ("Executor function required in promise constructor")

  Is executor NOT a function -> throw error ("Executor must be callable") 
*/
JsVar *jswrap_promise_constructor(JsVar *executor) {
  jsiConsolePrintf("jswrap_promise_constructor()\n");
  JsVar *obj = jspromise_create();
  if (obj) {
    // create resolve and reject
    JsVar *args[2] = {
        jsvNewNativeFunction((void (*)(void))_jswrap_promise_queueresolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS)),
        jsvNewNativeFunction((void (*)(void))_jswrap_promise_queuereject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS))
    };
    // bind 'this' to functions
    // so that when resolve() is called, its called, this points to newly created Promise.
    if (args[0]) jsvObjectSetChild(args[0], JSPARSE_FUNCTION_THIS_NAME, obj);
    if (args[1]) jsvObjectSetChild(args[1], JSPARSE_FUNCTION_THIS_NAME, obj);
    // call the executor

    //EMULATES A TRY BLOCK
    JsExecFlags oldExecute = execInfo.execute;
    if (executor) jsvUnLock(jspeFunctionCall(executor, 0, obj, false, 2, args));
    execInfo.execute = oldExecute;
    //Ah, it means to not let the errors be REAL errors.(affect flow)
    jsvUnLockMany(2, args);

    //errors...
    JsVar *exception = jspGetException();
    if (exception) {
      _jswrap_promise_queuereject(obj, exception);
      jsvUnLock(exception);
    }
  }
  return obj;
}

JsVar *jswrap_promise_all(JsVar *arr) {
  jsiConsolePrintf("jswrap_promise_all()\n");
  if (!jsvIsIterable(arr)) {
    jsExceptionHere(JSET_TYPEERROR, "Expecting something iterable, got %t", arr);
    return 0;
  }
  JsVar *promise = jspNewObject(0, "Promise");
  if (!promise) return 0;
  JsVar *reject = jsvNewNativeFunction((void (*)(void))jswrap_promise_all_reject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
  if (!reject) return promise; // out of memory error
  jsvObjectSetChild(reject, JSPARSE_FUNCTION_THIS_NAME, promise); // bind 'this'
  JsVar *promiseResults = jsvNewEmptyArray();
  int promiseIndex = 0;
  int promisesComplete = 0;
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, arr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *p = jsvObjectIteratorGetValue(&it);
    if (_jswrap_promise_is_promise(p)) {
      JsVar *resolve = jsvNewNativeFunction((void (*)(void))jswrap_promise_all_resolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS)|(JSWAT_JSVAR<<(JSWAT_BITS*2)));
      // NOTE: we use (this,JsVar,JsVar) rather than an int to avoid #2377 on emscripten, since that argspec is already used for forEach/otehrs
      // bind the index variable
      JsVar *indexVar = jsvNewFromInteger(promiseIndex);
      jsvAddFunctionParameter(resolve, 0, indexVar);
      jsvUnLock(indexVar);
      jsvObjectSetChild(resolve, JSPARSE_FUNCTION_THIS_NAME, promise); // bind 'this'
      jsvUnLock2(jswrap_promise_then(p, resolve, reject), resolve);
    } else {
      jsvSetArrayItem(promiseResults, promiseIndex, p);
      promisesComplete++;
    }
    jsvUnLock(p);
    promiseIndex++;
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  if (promisesComplete==promiseIndex) { // already all sorted - return a resolved promise
    jsvUnLock(promise);
    promise = jswrap_promise_resolve(promiseResults);
    jsvUnLock(promiseResults);
  } else { // return our new promise that will resolve when everything is done
    jsvObjectSetChildAndUnLock(promise, JS_PROMISE_REMAINING_NAME, jsvNewFromInteger(promiseIndex-promisesComplete));
    jsvObjectSetChildAndUnLock(promise, JS_PROMISE_RESULT_NAME, promiseResults);
  }

  jsvUnLock(reject);
  return promise;
}


/*JSON{
  "type" : "staticmethod",
  "class" : "Promise",
  "name" : "resolve",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_resolve",
  "params" : [
    ["promises","JsVar","Data to pass to the `.then` handler"]
  ],
  "return" : ["JsVar","A new Promise"],
  "typescript": "resolve<T extends any>(promises: T): Promise<T>;"
}
Return a new promise that is already resolved (at idle it'll call `.then`)
*/

/*
  Promise.resolve()
*/
JsVar *jswrap_promise_resolve(JsVar *data) {
  jsiConsolePrintf("jswrap_promise_resolve()\n");
  JsVar *promise = 0;
  // return the promise passed as value, if the value was a promise object.
  if (_jswrap_promise_is_promise(data))
    return jsvLockAgain(data);
  // If the value is a thenable (i.e. has a "then" method), the
  // returned promise will "follow" that thenable, adopting its eventual state
  if (jsvIsObject(data)) {
    JsVar *then = jsvObjectGetChildIfExists(data,"then");
    if (jsvIsFunction(then))
      promise = jswrap_promise_constructor(then);
    jsvUnLock(then);
    if (promise) return promise;
  }
  // otherwise the returned promise will be fulfilled with the value.
  promise = jspromise_create();
  if (!promise) return 0;
  jspromise_resolve(promise, data);
  return promise;
}

/*JSON{
  "type" : "staticmethod",
  "class" : "Promise",
  "name" : "reject",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_reject",
  "params" : [
    ["promises","JsVar","Data to pass to the `.catch` handler"]
  ],
  "return" : ["JsVar","A new Promise"]
}
Return a new promise that is already rejected (at idle it'll call `.catch`)
*/
/*
  Promise.reject()
*/
JsVar *jswrap_promise_reject(JsVar *data) {
  jsiConsolePrintf("jswrap_promise_reject()\n");
  JsVar *promise = jspromise_create();
  if (!promise) return 0;
  jspromise_reject(promise, data);
  return promise;
}

// Next promise created by .then or .catch
void _jswrap_promise_add(JsVar *parent, JsVar * next, JsVar *callback, bool resolve) {
  jsiConsolePrintf("_jswrap_promise_add()\n");
  if (!jsvIsFunction(callback)) {
    jsExceptionHere(JSET_TYPEERROR, "Callback is not a function");
    return;
  }
  bool resolveImmediately = false;
  JsVar *resolveImmediatelyValue = 0;
  if (resolve) {
    /* Note: we use jsvFindChildFromString not ObjectGetChild so we get the name.
     * If we didn't then we wouldn't know if it was resolved, but with undefined */
    JsVar *resolved = jsvFindChildFromString(parent, JS_PROMISE_RESOLVED_NAME);
    if (resolved) {
      //The promise is already resolved... so...
      resolveImmediately = true;
      resolveImmediatelyValue = jsvSkipNameAndUnLock(resolved);
    }
  }
  
  // Binding to next promise
  //We set it to be an object, of type { cb: , next:}
  //.thn = {cb:,next:}
  //.cat = {cb:,next:}
  JsVar *reaction = jsvNewObject();
  jsvObjectSetChild(reaction, "cb", callback);
  jsvObjectSetChild(reaction, "next", next);

  // Saving to promise
  const char *name = resolve ? JS_PROMISE_THEN_NAME : JS_PROMISE_CATCH_NAME;
  JsVar *c = jsvObjectGetChildIfExists(parent, name);
  if (!c) {
    // Initially unset, set .thn .cat
    jsvObjectSetChildAndUnLock(parent, name, reaction);
  } else {
    // Append
    if (jsvIsArray(c)) {
      //If its an array, append to it.
      jsvArrayPush(c, reaction);
      jsvUnlock(reaction);
    } else {
      //Its not an array.
      //Turn it into an array, to store > 1 callbacks.
      //Current value paired into an array with new value.
      JsVar *fns[2] = {c,reaction};
      JsVar *arr = jsvNewArray(fns, 2);
      jsvObjectSetChildAndUnLock(parent, name, arr);
    }
    jsvUnLock(c);
  }
  if (resolveImmediately) { // If so, queue a resolve event
    _jswrap_promise_queueresolve(parent, resolveImmediatelyValue);
    jsvUnLock(resolveImmediatelyValue);
  }
}


/*
  Add callback handlers...
  Create and return new promise, saved in parent.chain

  This should actually always return a new promise
  because they are independent chains.
  =================================================================================================
=================================================================================================
  =======================================THEN()=============================================================
  =================================================================================================
  =================================================================================================

  --SHOULD--
  ensure this == promise?
  cbs are callbable?, else set to undefined.
  associate the newly created promise with the callbacks, so that when the callbacks are fired, they can retrieve the promise.

  check for pending to push callbacks to fullfilled/rejected arrays
  check for fulfilled to fire the callback ...
  check for rejected to fire the callback ...

  set ishandled

  return a new promise always.

  --DOES NOT--
  return a new promise always.
  call error if already errored? -- no such thing as late error handling, error would have thrown, unpreventable.
  saves handlers even if it calls them, un-necessary?
  should allow callback to be not a function, .then(undefined,catch) is valid.
  could implement microqueue so that promise has priority

  --DOES--
  checks if callback is a function -> throws error.
  sets prom.chain , which is the association of the callback to the newly created promise.
  saves handlers
  calls callback if already resolved.
*/

/*JSON{
  "type" : "method",
  "class" : "Promise",
  "name" : "then",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_then",
  "params" : [
    ["onFulfilled","JsVar","A callback that is called when this promise is resolved"],
    ["onRejected","JsVar","[optional] A callback that is called when this promise is rejected (or nothing)"]
  ],
  "return" : ["JsVar","The original Promise"],
  "typescript": "then<TResult1 = T, TResult2 = never>(onfulfilled?: ((value: T) => TResult1 | Promise<TResult1>) | undefined | null, onrejected?: ((reason: any) => TResult2 | Promise<TResult2>) | undefined | null): Promise<TResult1 | TResult2>;"
}
 */
JsVar *jswrap_promise_then(JsVar *parent, JsVar *onFulfilled, JsVar *onRejected) {
  jsiConsolePrintf("jswrap_promise_then()\n");
  JsVar *resultingProm = jspNewObject(0, "Promise");
  

  //allow undefined for pass-through.
  if (onFulfilled)
    _jswrap_promise_add(parent, resultingProm, onFulfilled, true);
  if (onRejected)
    _jswrap_promise_add(parent, resultingProm, onRejected, false);
  
  return resultingProm;
}


/*JSON{
  "type" : "method",
  "class" : "Promise",
  "name" : "catch",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_catch",
  "params" : [
    ["onRejected","JsVar","A callback that is called when this promise is rejected"]
  ],
  "return" : ["JsVar","The original Promise"]
}
 */
JsVar *jswrap_promise_catch(JsVar *parent, JsVar *onRejected) {
  jsiConsolePrintf("jswrap_promise_catch()\n");
  JsVar *resultingProm = jspNewObject(0, "Promise");

  //allow undefined for pass-through.
  _jswrap_promise_add(parent, resultingProm, onRejected, false);

  return resultingProm;
}
#endif // ESPR_NO_PROMISES
