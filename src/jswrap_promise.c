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
void _jswrap_promise_add(JsVar *parent, JsVar *callback, bool resolve);

bool _jswrap_promise_is_promise(JsVar *promise) {
  JsVar *constr = jspGetConstructor(promise);
  bool isPromise = constr && (void*)constr->varData.native.ptr==(void*)jswrap_promise_constructor;
  jsvUnLock(constr);
  return isPromise;
}

/*
STAGE 2 - PERFORM ACTION

promise - which promise we are resolving/rejecting
data - the value/error we are resolving/rejecting with
fn - the user registered callbacks passed to then/catch

Executes Settled/Rejected handlers, fn, can be array due to multiple .then calls in unchained manner.

If there is exception, it rejects the promise and returns.
else:
Resolves the .then() shell promise accordingly.

*/
void _jswrap_promise_resolve_or_reject(JsVar *promise, JsVar *data, JsVar *fn) {
  // remove any existing handlers since we already have them in `fn`
  // If while we're iterating below a function re-adds to the chain then
  // we can execute that later
  // https://github.com/espruino/Espruino/issues/894#issuecomment-402553934
  jsvObjectRemoveChild(promise, JS_PROMISE_THEN_NAME); // remove 'resolve' and 'reject' handlers
  jsvObjectRemoveChild(promise, JS_PROMISE_CATCH_NAME); // remove 'resolve' and 'reject' handlers
  JsVar *chainedPromise = jsvObjectGetChildIfExists(promise, "chain");
  jsvObjectRemoveChild(promise, "chain"); // unlink chain
  // execute handlers from `fn`
  JsVar *result = 0;
  if (jsvIsArray(fn)) {
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, fn);
    bool first = true;
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *f = jsvObjectIteratorGetValue(&it);
      JsVar *v = jspExecuteFunction(f, promise, 1, &data);
      if (first) {
        first = false;
        result = v;
      } else jsvUnLock(v);
      jsvUnLock(f);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
  } else if (fn) {
    result = jspExecuteFunction(fn, promise, 1, &data);
  }

  JsVar *exception = jspGetException();
  if (exception) {
    _jswrap_promise_queuereject(chainedPromise, exception);
    jsvUnLock3(exception, result, chainedPromise);
    return;
  }

  //Update the new returned promise's state/value
  if (chainedPromise) {
    //If we returned a promise...
    if (_jswrap_promise_is_promise(result)) {
      // points the child promise's settled/resolved firing
      // to resolve the outer promise.
      JsVar *fnres = jsvNewNativeFunction((void (*)(void))_jswrap_promise_queueresolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
      JsVar *fnrej = jsvNewNativeFunction((void (*)(void))_jswrap_promise_queuereject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
      if (fnres && fnrej) {
        //the callbacks' have their this set outer promise.
        jsvObjectSetChild(fnres, JSPARSE_FUNCTION_THIS_NAME, chainedPromise); // bind 'this'
        jsvObjectSetChild(fnrej, JSPARSE_FUNCTION_THIS_NAME, chainedPromise); // bind 'this'

        //But the 'this' of the callbacks will point to the outer-new Promise.
        //Equivalently calling .then(resolve,reject) on the inner-returned Promise
        //Thus creating more callbacks for it when it resolves.
        //When the inner-returned promise resolves its-self, it will trigger these callbacks, which will in turn resolve the outer-returned promise.

        //Add Fullfilled Handler - yet they are resolve()
        _jswrap_promise_add(result, fnres, true);
        //Add Rejected Handler - yet they are reject()
        _jswrap_promise_add(result, fnrej, false);
      }
      jsvUnLock2(fnres,fnrej);
    } else {
      //We returned a non-promise.
      _jswrap_promise_queueresolve(chainedPromise, result);
    }
  }
  jsvUnLock2(result, chainedPromise);
}
/*
  We have been requested to resolve/reject a promise.
  STAGE ONE - VALIDATE
  finds the nearest handler by iterating the chain.
*/
void _jswrap_promise_resolve_or_reject_chain(JsVar *promise, JsVar *data, bool resolve) {
  const char *eventName = resolve ? JS_PROMISE_THEN_NAME : JS_PROMISE_CATCH_NAME;
  if (_jswrap_promise_is_promise(data)) {
    jsExceptionHere(JSET_ERROR, "Resolving a Promise with a value that is a Promise is not currently supported");
    return;
  }
  // find the nearest handler by iterating chain
  JsVar *fn = jsvObjectGetChildIfExists(promise, eventName);
  if (!fn) {
    JsVar *chainedPromise = jsvObjectGetChildIfExists(promise, "chain");
    while (chainedPromise) {
      fn = jsvObjectGetChildIfExists(chainedPromise, eventName);
      if (fn) {
        //We have found a handler! By doing so, we have made the chain smaller.
        _jswrap_promise_resolve_or_reject(chainedPromise, data, fn);
        jsvUnLock2(fn, chainedPromise);
        return;
      }
      JsVar *n = jsvObjectGetChildIfExists(chainedPromise, "chain");
      jsvUnLock(chainedPromise);
      chainedPromise = n;
    }
  }
  //fn=true = Found handler in THIS parent.
  //fn=false = No handler found at all.

  //========================
  //Sets resolved.!
  //========================
  if (resolve)
    jsvObjectSetChild(promise, JS_PROMISE_RESOLVED_NAME, data);
  if (fn) {
    //There was initial handler for THIS promise.
    _jswrap_promise_resolve_or_reject(promise, data, fn);
    jsvUnLock(fn);
  } else if (!resolve) {
    //No handler found! Thus it is unhandled.
    //A promise has been rejected, but no handler set.
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
*/
void _jswrap_promise_resolve(JsVar *promise, JsVar *data) {
  _jswrap_promise_resolve_or_reject_chain(promise, data, true);
}
/*
  QUEUE STAGE - RESOLVE
  find handler, run handler, update shell promise. (p1.chain)
*/
void _jswrap_promise_queueresolve(JsVar *promise, JsVar *data) {
  JsVar *fn = jsvNewNativeFunction((void (*)(void))_jswrap_promise_resolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS));
  if (!fn) return;
  jsvObjectSetChild(fn, JSPARSE_FUNCTION_THIS_NAME, promise); // bind 'this'
  jsiQueueEvents(promise, fn, &data, 1);
  jsvUnLock(fn);
}
/*
  REJECT WRAPPER
  find handler, run handler, update shell promise. (p1.chain)
*/
void _jswrap_promise_reject(JsVar *promise, JsVar *data) {
  _jswrap_promise_resolve_or_reject_chain(promise, data, false);
}
/*
  QUEUE STAGE - REJECT
  find handler, run handler, update shell promise. (p1.chain)
*/
void _jswrap_promise_queuereject(JsVar *promise, JsVar *data) {
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
  The internal resolve function.
*/
/// Resolve the given promise
void jspromise_resolve(JsVar *promise, JsVar *data) {
  _jswrap_promise_queueresolve(promise, data);
}
/*
  The internal reject function.
*/
/// Reject the given promise
void jspromise_reject(JsVar *promise, JsVar *data) {
  _jswrap_promise_queuereject(promise, data);
}


/*JSON{
  "type" : "constructor",
  "class" : "Promise",
  "name" : "Promise",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_constructor",
  "params" : [
    ["executor","JsVar","A function of the form `function (resolve, reject)`"]
  ],
  "return" : ["JsVar","A Promise"],
  "typescript": "new<T>(executor: (resolve: (value: T) => void, reject: (reason?: any) => void) => void): Promise<T>;"
}
Create a new Promise. The executor function is executed immediately (before the
constructor even returns) and
 */
JsVar *jswrap_promise_constructor(JsVar *executor) {
  JsVar *obj = jspromise_create();
  if (obj) {
    // create resolve and reject
    JsVar *args[2] = {
        jsvNewNativeFunction((void (*)(void))_jswrap_promise_queueresolve, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS)),
        jsvNewNativeFunction((void (*)(void))_jswrap_promise_queuereject, JSWAT_VOID|JSWAT_THIS_ARG|(JSWAT_JSVAR<<JSWAT_BITS))
    };
    // bind 'this' to functions
    if (args[0]) jsvObjectSetChild(args[0], JSPARSE_FUNCTION_THIS_NAME, obj);
    if (args[1]) jsvObjectSetChild(args[1], JSPARSE_FUNCTION_THIS_NAME, obj);
    // call the executor
    JsExecFlags oldExecute = execInfo.execute;
    if (executor) jsvUnLock(jspeFunctionCall(executor, 0, obj, false, 2, args));
    execInfo.execute = oldExecute;
    jsvUnLockMany(2, args);
    JsVar *exception = jspGetException();
    if (exception) {
      _jswrap_promise_queuereject(obj, exception);
      jsvUnLock(exception);
    }
  }
  return obj;
}

/*JSON{
  "type" : "staticmethod",
  "class" : "Promise",
  "name" : "all",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_promise_all",
  "params" : [
    ["promises","JsVar","An array of promises"]
  ],
  "return" : ["JsVar","A new Promise"],
  "typescript": "all(promises: Promise<any>[]): Promise<void>;"
}
Return a new promise that is resolved when all promises in the supplied array
are resolved.
*/
JsVar *jswrap_promise_all(JsVar *arr) {
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
  JsVar *promise = jspromise_create();
  if (!promise) return 0;
  jspromise_reject(promise, data);
  return promise;
}

/*
  .then calls this.
  Append onSettled handler.
*/
void _jswrap_promise_add(JsVar *parent, JsVar *callback, bool resolve) {
  if (!jsvIsFunction(callback)) {
    jsExceptionHere(JSET_TYPEERROR, "Callback is not a function");
    return;
  }

  bool resolveImmediately = false;
  JsVar *resolveImmediatelyValue = 0;

  //Are we adding an onSettled callback?
  if (resolve) {
    // Check to see if promise has already been resolved
    /* Note: we use jsvFindChildFromString not ObjectGetChild so we get the name.
     * If we didn't then we wouldn't know if it was resolved, but with undefined */
    JsVar *resolved = jsvFindChildFromString(parent, JS_PROMISE_RESOLVED_NAME);
    if (resolved) {
      //The promise is already resolved... so...
      resolveImmediately = true;
      resolveImmediatelyValue = jsvSkipNameAndUnLock(resolved);
    }
  }
  //Save Handlers.
  const char *name = resolve ? JS_PROMISE_THEN_NAME : JS_PROMISE_CATCH_NAME;
  JsVar *c = jsvObjectGetChildIfExists(parent, name);
  if (!c) {
    //No handler is set. Set one.
    jsvObjectSetChild(parent, name, callback);
  } else {
    //Handler is set.
    if (jsvIsArray(c)) {
      //If its an array, append to it.
      jsvArrayPush(c, callback);
    } else {
      //Its not an array.
      //Turn it into an array, to store > 1 callbacks.
      JsVar *fns[2] = {c,callback};
      JsVar *arr = jsvNewArray(fns, 2);
      jsvObjectSetChild(parent, name, arr);
      jsvUnLock(arr);
    }
    jsvUnLock(c);
  }
  //Should the callback be instantly fired???
  //Usually this would get queued by the asynchronous executor part of the promise, but if the promise was synchronous and instantly resolved, the queueResolve function would had fired, but done nothing because there was no handler/callback attached to it.  This situation is rare because the .then() would have to be called in an asynchronous callback to be called later than the queueResolve call.
  //Is there a chance this is called twice?
  if (resolveImmediately) { // If so, queue a resolve event
    //resolve here, means fire callback from idle.
    _jswrap_promise_queueresolve(parent, resolveImmediatelyValue);
    jsvUnLock(resolveImmediatelyValue);
  }
}

/*
  Create a new promise when returning from .then()
  Store it in parent.chain
*/
static JsVar *jswrap_promise_get_chained_promise(JsVar *parent) {
  JsVar *chainedPromise = jsvObjectGetChildIfExists(parent, "chain");
  if (!chainedPromise) {
    chainedPromise = jspNewObject(0, "Promise");
    jsvObjectSetChild(parent, "chain", chainedPromise);
  }
  return chainedPromise;
}

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

/*
  Add callback handlers...
  Create and return new promise, saved in parent.chain
*/
JsVar *jswrap_promise_then(JsVar *parent, JsVar *onFulfilled, JsVar *onRejected) {
  _jswrap_promise_add(parent, onFulfilled, true);
  if (onRejected)
    _jswrap_promise_add(parent, onRejected, false);
  return jswrap_promise_get_chained_promise(parent);
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
/*
  Same as above
*/
JsVar *jswrap_promise_catch(JsVar *parent, JsVar *onRejected) {
  _jswrap_promise_add(parent, onRejected, false);
  return jswrap_promise_get_chained_promise(parent);
}
#endif // ESPR_NO_PROMISES
