#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#define INVALID_HANDLE -1

/* JS functions */
/* See js/mruby_js.js file for the possible values of ret_allow_object */
extern void js_call(mrb_state *mrb, mrb_int handle, const char *name,
                    mrb_value *argv, int argc,
                    mrb_value* ret, int constructor_call);
extern void js_get_field(mrb_state *mrb, mrb_int handle, const char *field_name_p,
                         mrb_value *ret);
extern void js_get_root_object(mrb_state *mrb, mrb_value *ret);
extern void js_release_object(mrb_state *mrb, mrb_int handle);

static struct RClass *mjs_mod;
static struct RClass *js_obj_cls;

/* Object handle is stored as RData in mruby to leverage auto-dfree calls,
 * in other words, we are simulating finalizers here.
 */
static void
mruby_js_object_handle_free(mrb_state *mrb, void *p)
{
  mrb_int* handle = (mrb_int*) p;
  if (handle) {
    js_release_object(mrb, *handle);
  }
  free(p);
}

static const struct mrb_data_type mruby_js_object_handle_type ={
  "mruby_js_object_handle", mruby_js_object_handle_free
};

/* Gets the object handle value from a JsObject, it is put here since
 * mruby_js_set_object_handle uses this function.
 */
static mrb_int
mruby_js_get_object_handle_value(mrb_state *mrb, mrb_value js_obj)
{
  mrb_value value_handle;
  mrb_int* handle_p = NULL;

  value_handle = mrb_iv_get(mrb, js_obj, mrb_intern(mrb, "handle"));
  Data_Get_Struct(mrb, value_handle, &mruby_js_object_handle_type, handle_p);
  if (handle_p == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Cannot get handle value!");
  }

  return *handle_p;
}

/* bridge functions between JS side and C side */
int mruby_js_argument_type(mrb_state *mrb, mrb_value* argv, int idx)
{
  enum mrb_vtype t = mrb_type(argv[idx]);
  switch (t) {
    case MRB_TT_FALSE:
      return 0;
    case MRB_TT_TRUE:
      return 1;
    case MRB_TT_FIXNUM:
      return 2;
    case MRB_TT_FLOAT:
      return 3;
    case MRB_TT_OBJECT:
      return 4;
    case MRB_TT_STRING:
      return 5;
    default:
      mrb_raisef(mrb, E_ARGUMENT_ERROR,
                 "Given type %d is not supported in JavaScript!\n", t);
  }
  /* This is not reachable */
  return -1;
}

char* mruby_js_get_string(mrb_state *mrb, mrb_value* argv, int idx)
{
  struct RString *s;
  /* TODO: well, let's come back to the auto-conversion later,
   * since that involves changing the mruby_js_argument_type function
   * as well.
   */
  if (mrb_type(argv[idx]) != MRB_TT_STRING) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Given argument is not a string!");
  }
  s = mrb_str_ptr(argv[idx]);
  if (strlen(s->ptr) != s->len) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "String contains NULL!");
  }
  return s->ptr;
}

mrb_int mruby_js_get_integer(mrb_state *mrb, mrb_value* argv, int idx)
{
  if (mrb_type(argv[idx]) != MRB_TT_FIXNUM) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Given argument is not an integer!");
  }
  return mrb_fixnum(argv[idx]);
}

mrb_float mruby_js_get_float(mrb_state *mrb, mrb_value* argv, int idx)
{
  if (mrb_type(argv[idx]) != MRB_TT_FLOAT) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Given argument is not a float!");
  }
  return mrb_float(argv[idx]);
}

mrb_int mruby_js_get_object_handle(mrb_state *mrb, mrb_value* argv, int idx)
{
  if (mrb_type(argv[idx]) != MRB_TT_OBJECT) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Given argument is not an object!");
  }

  /* currently we only support passing objects of JsObject type */
  if (mrb_object(argv[idx])->c != js_obj_cls) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Object argument must be JsObject type!");
  }

  return mruby_js_get_object_handle_value(mrb, argv[idx]);
}

void mruby_js_name_error(mrb_state *mrb)
{
  mrb_raise(mrb, E_ARGUMENT_ERROR, "Error occurs when locating the function to call!");
}

void mruby_js_set_integer(mrb_state *mrb, mrb_value* arg, mrb_int val)
{
  *arg = mrb_fixnum_value(val);
}

void mruby_js_set_float(mrb_state *mrb, mrb_value* arg, mrb_float val)
{
  *arg = mrb_float_value(val);
}

void mruby_js_set_boolean(mrb_state *mrb, mrb_value* arg, int val)
{
  *arg = (val == 1) ? (mrb_true_value()) : (mrb_false_value());
}

void mruby_js_set_nil(mrb_state *mrb, mrb_value* arg)
{
  *arg = mrb_nil_value();
}

void mruby_js_set_string(mrb_state *mrb, mrb_value* arg, const char* val)
{
  *arg = mrb_str_new_cstr(mrb, val);
}

void mruby_js_set_object_handle(mrb_state *mrb, mrb_value* arg, mrb_int handle)
{
  struct RObject *o;
  enum mrb_vtype ttype = MRB_INSTANCE_TT(js_obj_cls);
  mrb_value argv;

  if (ttype == 0) ttype = MRB_TT_OBJECT;
  o = (struct RObject*)mrb_obj_alloc(mrb, ttype, js_obj_cls);
  *arg = mrb_obj_value(o);
  argv = mrb_fixnum_value(handle);
  mrb_funcall_argv(mrb, *arg, mrb->init_sym, 1, &argv);
}

/* mrb functions */

static mrb_value
mrb_js_get_root_object(mrb_state *mrb, mrb_value mod)
{
  mrb_sym root_sym = mrb_intern(mrb, "ROOT_OBJECT");
  mrb_value ret = mrb_iv_get(mrb, mod, root_sym);
  if (!mrb_nil_p(ret)) {
    return ret;
  }

  js_get_root_object(mrb, &ret);
  if (!mrb_nil_p(ret)) {
    /* Cache root object to ensure singleton */
    mrb_iv_set(mrb, mod, root_sym, ret);
  }

  return ret;
}

static mrb_value
mrb_js_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_int handle = INVALID_HANDLE;
  mrb_int *handle_p;

  mrb_get_args(mrb, "i", &handle);
  if (handle <= 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "No valid handle is provided!");
  }

  handle_p = (mrb_int*) malloc(sizeof(mrb_int));
  if (handle_p == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot allocate memory!");
  }
  *handle_p = handle;
  mrb_iv_set(mrb, self, mrb_intern(mrb, "handle"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
                       &mruby_js_object_handle_type, (void*) handle_p)));

  return self;
}

static mrb_value
mrb_js_call_with_option(mrb_state *mrb, mrb_value self, int constructor_call)
{
  char* name = NULL;
  mrb_value *argv = NULL;
  mrb_value ret = mrb_nil_value();
  int argc = 0;

  /* TODO: proc handling */
  mrb_get_args(mrb, "z*", &name, &argv, &argc);
  if (name == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Field name not provided!");
  }

  js_call(mrb, mruby_js_get_object_handle_value(mrb, self),
          name, argv, argc, &ret, constructor_call);
  return ret;
}

static mrb_value
mrb_js_call(mrb_state *mrb, mrb_value self)
{
  return mrb_js_call_with_option(mrb, self, 0);
}

static mrb_value
mrb_js_call_constructor(mrb_state *mrb, mrb_value self)
{
  return mrb_js_call_with_option(mrb, self, 1);
}

static mrb_value
mrb_js_get(mrb_state* mrb, mrb_value self)
{
  char* name = NULL;
  mrb_value ret = mrb_nil_value();

  mrb_get_args(mrb, "z", &name);
  if (name == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Field name not provided!");
  }

  js_get_field(mrb, mruby_js_get_object_handle_value(mrb, self),
               name, &ret);
  return ret;
}

mrb_value
mrb_js_funcall_argv(mrb_state *mrb, const char *func, int argc, mrb_value *argv)
{
  mrb_value mjs, jso, r;

//printf("mrb_js_funcall_argv() start.\n");
  mjs = mrb_const_get(mrb, mrb_obj_value(mrb->object_class), mrb_intern(mrb, "MrubyJs"));

  jso = mrb_js_get_root_object(mrb, mjs);
//printf("JsObject: ");
//mrb_p(mrb, jso);

  js_call(mrb, mruby_js_get_object_handle_value(mrb, jso),
          func, argv, argc, &r, 0);

//printf("mrb_js_funcall_argv() end.\n");

  return r;
}

#ifndef MRB_FUNCALL_ARGC_MAX
#define MRB_FUNCALL_ARGC_MAX 16
#endif

mrb_value
mrb_js_funcall(mrb_state *mrb, const char *func, int argc, ...)
{
  va_list ap;
  int i;
  mrb_value r;

printf("mrb_js_funcall() start.\n");
  if (argc == 0) {
    r = mrb_js_funcall_argv(mrb, func, 0, 0);
  }
  else if (argc == 1) {
    mrb_value v;

    va_start(ap, argc);
    v = va_arg(ap, mrb_value);
    va_end(ap);
    r = mrb_js_funcall_argv(mrb, func, 1, &v);
  }
  else {
    mrb_value argv[MRB_FUNCALL_ARGC_MAX];

    if (argc > MRB_FUNCALL_ARGC_MAX) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "Too long arguments. (limit=%d)", MRB_FUNCALL_ARGC_MAX);
    }

    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
      argv[i] = va_arg(ap, mrb_value);
    }
    va_end(ap);
    r = mrb_js_funcall_argv(mrb, func, argc, argv);
  }
printf("mrb_js_funcall() end.\n");
  return r;
}

void
mrb_mruby_js_gem_init(mrb_state* mrb) {
  mjs_mod = mrb_define_module(mrb, "MrubyJs");
  mrb_define_class_method(mrb, mjs_mod, "get_root_object", mrb_js_get_root_object, ARGS_NONE());

  js_obj_cls = mrb_define_class_under(mrb, mjs_mod, "JsObject", mrb->object_class);
  mrb_define_method(mrb, js_obj_cls, "initialize", mrb_js_initialize, ARGS_REQ(1));

  mrb_define_method(mrb, js_obj_cls, "call", mrb_js_call, ARGS_ANY());
  mrb_define_method(mrb, js_obj_cls, "call_constructor", mrb_js_call_constructor, ARGS_ANY());
  mrb_define_method(mrb, js_obj_cls, "get", mrb_js_get, ARGS_REQ(1));
}

void
mrb_mruby_js_gem_final(mrb_state* mrb) {
}
