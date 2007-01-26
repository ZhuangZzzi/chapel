/*** normalize
 ***
 *** This pass and function normalizes parsed and scope-resolved AST.
 ***/

#include "astutil.h"
#include "build.h"
#include "expr.h"
#include "passes.h"
#include "runtime.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"
#include "symscope.h"

bool normalized = false;

static void change_method_into_constructor(FnSymbol* fn);
static void enable_scalar_promotion(FnSymbol* fn);
static void iterator_transform(FnSymbol* fn);
static void build_lvalue_function(FnSymbol* fn);
static void normalize_returns(FnSymbol* fn);
static void call_constructor_for_class(CallExpr* call);
static void decompose_special_calls(CallExpr* call);
static void hack_resolve_types(Expr* expr);
static void apply_getters_setters(FnSymbol* fn);
static void insert_call_temps(CallExpr* call);
static void fix_user_assign(CallExpr* call);
static void fix_def_expr(VarSymbol* var);
static void tag_global(FnSymbol* fn);
static void fixup_array_formals(FnSymbol* fn);
static void clone_parameterized_primitive_methods(FnSymbol* fn);
static void fixup_parameterized_primitive_formals(FnSymbol* fn);


void normalize(void) {
  forv_Vec(ModuleSymbol, mod, allModules) {
    normalize(mod);
  }
  normalized = true;
}

void normalize(BaseAST* base) {
  Vec<BaseAST*> asts;

  asts.clear();
  collect_asts( &asts, base);
  forv_Vec(BaseAST, ast, asts) {
    if (FnSymbol* fn = dynamic_cast<FnSymbol*>(ast)) {
      currentLineno = fn->lineno;
      currentFilename = fn->filename;
      fixup_array_formals(fn);
      clone_parameterized_primitive_methods(fn);
      fixup_parameterized_primitive_formals(fn);
      if (fn->fnClass == FN_ITERATOR) {
        enable_scalar_promotion( fn);
        iterator_transform( fn);
      }
      change_method_into_constructor(fn);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    if (FnSymbol* fn = dynamic_cast<FnSymbol*>(ast)) {
      currentLineno = fn->lineno;
      currentFilename = fn->filename;
      fixup_array_formals(fn);
      fixup_parameterized_primitive_formals(fn);
      if (fn->buildSetter)
        build_lvalue_function(fn);
    }
  }

  asts.clear();
  collect_asts(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    if (FnSymbol* fn = dynamic_cast<FnSymbol*>(ast)) {
      normalize_returns(fn);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    currentLineno = ast->lineno;
    currentFilename = ast->filename;
    if (CallExpr* a = dynamic_cast<CallExpr*>(ast)) {
      call_constructor_for_class(a);
      decompose_special_calls(a);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    currentLineno = ast->lineno;
    currentFilename = ast->filename;
    if (FnSymbol* a = dynamic_cast<FnSymbol*>(ast))
      if (!a->defSetGet)
        apply_getters_setters(a);
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    currentLineno = ast->lineno;
    currentFilename = ast->filename;
    if (DefExpr* a = dynamic_cast<DefExpr*>(ast)) {
      if (VarSymbol* var = dynamic_cast<VarSymbol*>(a->sym))
        if (dynamic_cast<FnSymbol*>(a->parentSymbol))
          fix_def_expr(var);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    currentLineno = ast->lineno;
    currentFilename = ast->filename;
    if (CallExpr* a = dynamic_cast<CallExpr*>(ast)) {
      insert_call_temps(a);
      fix_user_assign(a);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    if (FnSymbol *fn = dynamic_cast<FnSymbol*>(ast)) {
      tag_global(fn);
    }
  }

  asts.clear();
  collect_asts_postorder(&asts, base);
  forv_Vec(BaseAST, ast, asts) {
    currentLineno = ast->lineno;
    currentFilename = ast->filename;
    if (Expr* a = dynamic_cast<Expr*>(ast)) {
      hack_resolve_types(a);
    }
  }
}


// Create formals for iterator class methods/functions and set _this.
static void
iterator_formals( FnSymbol *fn, ClassType *t, ArgSymbol *cursor=NULL) {
  fn->insertFormalAtTail( new DefExpr( new ArgSymbol( INTENT_BLANK,
                                                         "_yummyMethodToken",
                                                         dtMethodToken)));
  fn->_this = new ArgSymbol( INTENT_BLANK, "this", t);
  fn->insertFormalAtTail( new DefExpr( fn->_this));
  if (cursor) fn->insertFormalAtTail( new DefExpr( cursor));
}


// Create a field in the class for each local variable and replace uses.
static void
iterator_create_fields( FnSymbol *fn, ClassType *ic) {
  int uid = 0;
  ArgSymbol* _this = new ArgSymbol(INTENT_BLANK, "this", ic);

  // create a field for each formal
  for_formals(formal, fn) {
    formal->defPoint->remove();
    if (formal->type == dtMethodToken)
      continue;
    formal->name = stringcat("_", intstring(uid++), "_", formal->name);
    consType const_type = (formal->intent==INTENT_PARAM) ? VAR_PARAM : VAR_VAR;
    VarSymbol *newfield = new VarSymbol(formal->name,
                                        formal->type,
                                        VAR_NORMAL,
                                        const_type);
    ic->fields->insertAtTail(
      new DefExpr(newfield, NULL, formal->defPoint->exprType));
    // replace uses in body
    forv_Vec(SymExpr, se, formal->uses) {
      se->replace(new CallExpr(".", _this, new_StringSymbol(se->var->name)));
    }
  }


  // create a field for each local
  Vec<BaseAST*> children;
  collect_asts(&children, fn->body);
  forv_Vec(BaseAST, ast, children) {
    if (DefExpr *def = dynamic_cast<DefExpr*>(ast)) {
      if (VarSymbol *v = dynamic_cast<VarSymbol*>(def->sym)) {
        if (v->isCompilerTemp)
          continue;

        Expr* def_init = def->init;
        Expr* def_type = def->exprType;
        def_init->remove();
        def_type->remove();

        v->name = stringcat("_", intstring(uid++), "_", v->name);
        v->cname = v->name;

        // need to reset default value (make re-entrant)
        if (!def_init)
          def_init = new CallExpr("_init", new CallExpr(".", _this, new_StringSymbol(v->name)));
        def->replace(new CallExpr("=", new CallExpr(".", _this, new_StringSymbol(v->name)), def_init));

        if (def_type) {
          ic->fields->insertAtTail(new DefExpr(v, NULL, def_type->copy()));
        } else {
          ic->fields->insertAtTail(new DefExpr(v, NULL, new CallExpr(PRIMITIVE_TYPEOF, def_init->copy())));
        }

        // replace uses in body
        compute_sym_uses( fn);
        forv_Vec( SymExpr, se, v->uses) {  // replace each use
          se->replace( new CallExpr( ".", _this, new_StringSymbol(se->var->name)));
        }
      }
    }
  }

  // create formals
  fn->insertFormalAtTail( new DefExpr( new ArgSymbol( INTENT_BLANK,
                                                      "_yummyMethodToken",
                                                      dtMethodToken)));
  fn->_this = _this;
  fn->insertFormalAtTail( new DefExpr( fn->_this));
}


static ArgSymbol*
iterator_find_arg( char *name, AList *formals) {
  for_alist( DefExpr, de, formals) {
    if (ArgSymbol *a = dynamic_cast<ArgSymbol*>(de->sym)) {
      if (!strcmp( name, a->name)) {
        return a;
      }
    }
  }
  return NULL;
}


// Replace call expressions that reference this with the arg symbol in
// formals.
static void
iterator_constructor_fixup( ClassType *ct) {
  FnSymbol *fn = ct->defaultConstructor;

  for_alist( DefExpr, de, fn->formals) {
    Vec<BaseAST*> asts;
    asts.clear();
    collect_asts( &asts, de);
    forv_Vec( BaseAST, ast, asts) {
      if (CallExpr *ce = dynamic_cast<CallExpr*>( ast)) {
        if (ce->argList->length() > 0) {
          if (SymExpr *arg1e = dynamic_cast<SymExpr*>( ce->argList->get(1))) {
            Symbol *arg1 = arg1e->var;
            if (!strcmp( arg1->name, "this") && 
                (arg1e->typeInfo() == ct)) {
              if (SymExpr *arg2e = dynamic_cast<SymExpr*>( ce->argList->get(2))) {
                Symbol *arg2 = dynamic_cast<VarSymbol*>(arg2e->var);
                char* str;
                if (!get_string(arg2e, &str))
                  INT_FATAL(arg2e, "string literal expected");
                ArgSymbol *a = iterator_find_arg(str, fn->formals);
                if (!a) INT_FATAL( arg2, "could not find arg to replace with");
                ce->replace( new SymExpr( a));
              }
            }
          }
        }
      }
    }
  }
}


// Replace yield statements with a return and label.  Return both a vec of the
// replaced return statements which will be used when constructing getValue 
// and a vec of labels (for cursor returns).
static void
iterator_replace_yields( FnSymbol *fn, 
                         Vec<ReturnStmt*>  *vals_returned,
                         Vec<LabelSymbol*> *labels) {
  uint return_pt= 0;
  Vec<BaseAST*> children;
  LabelSymbol *l= new LabelSymbol( stringcat("return_", intstring( return_pt)));
  labels->add( l);  // base case

  collect_asts( &children, fn->body);
  forv_Vec( BaseAST, ast, children) {
    if (ReturnStmt *rs=dynamic_cast<ReturnStmt*>( ast)) {
      if (rs->yield) {
        return_pt++;
        rs->insertBefore( new ReturnStmt( new_IntSymbol( return_pt)));
        l = new LabelSymbol( stringcat( "return_", intstring( return_pt)));
        labels->add( l);
        rs->insertAfter(new DefExpr(l));
        vals_returned->add( dynamic_cast<ReturnStmt*>(rs->remove()));
      }
    }
  }
}


// Build and insert the jump table for getNextCursor method
static void
iterator_build_jtable( FnSymbol *fn, ArgSymbol *c, Vec<ReturnStmt*> *vals_returned, Vec<LabelSymbol*> *labels) {
  BlockStmt *b= new BlockStmt();
  LabelSymbol *l = labels->first();
  b->insertAtTail(new DefExpr(l)); 
  for( int retpt=0; retpt<=vals_returned->length(); retpt++) {
    b->insertAtHead( new CondStmt( new CallExpr( "==", c, new_IntSymbol( retpt)), new GotoStmt( goto_normal, l)));
    labels->remove( 0);
    l = labels->first();
  }
  fn->body->insertAtHead( b);
  fn->body->insertAtTail( new ReturnStmt( new_IntSymbol( vals_returned->length()+1)));
}


// Build the return value jump table (i.e., getValue method)
static void
iterator_build_vtable( FnSymbol *fn, ArgSymbol *c, Vec<ReturnStmt*> *vals_returned) {
  BlockStmt *b= build_chpl_stmt();
  uint retpt= 1;
  forv_Vec(Expr, stmt, *vals_returned) {
    b->insertAtTail( new CondStmt( new CallExpr( "==", c, new_IntSymbol( retpt)),
                                   stmt));
    retpt++;
  }
  fn->body->insertAtHead( b);
}


static void
iterator_update_this_uses( FnSymbol *fn, DefExpr *newdef, DefExpr *olddef) {
  ArgSymbol *newsym = dynamic_cast<ArgSymbol*>( newdef->sym);
  ArgSymbol *oldsym = dynamic_cast<ArgSymbol*>( olddef->sym);
  ASTMap replace;
  replace.put( oldsym, newsym);
  update_symbols( fn, &replace);
}


static void
iterator_method( FnSymbol *fn) {
  fn->fnClass = FN_FUNCTION;
  fn->isMethod = true;                // method of iterator class
  fn->global = true;                  // other modules need access
  fn->retType = dtUnknown;            // let resolve figures these out
  //  fn->retExprType = NULL;
}


static void
iterator_transform( FnSymbol *fn) {
  static int uid = 0;
  ModuleSymbol*m = fn->getModule();
  char        *classn = stringcat("_iterator_", intstring(uid++), "_", fn->name);
  ClassType   *ic = new ClassType( CLASS_CLASS);
  TypeSymbol  *ict = new TypeSymbol( classn, ic);
  DefExpr     *ic_def = new DefExpr( ict);
  m->stmts->insertAtHead(ic_def);

  ArgSymbol *cursor = new ArgSymbol(INTENT_BLANK, "cursor", dtInt[INT_SIZE_64]);

  // create getNextCursor
  FnSymbol *nextcf = fn->copy();
  nextcf->name = nextcf->cname = canonicalize_string("getNextCursor");
  iterator_method( nextcf);
  m->stmts->insertAtHead(new DefExpr(nextcf));
  compute_sym_uses( nextcf);
  iterator_create_fields( nextcf, ic);
  nextcf->insertFormalAtTail( new DefExpr( cursor));
  Vec<ReturnStmt*> vals_returned;
  Vec<LabelSymbol*> labels;
  iterator_replace_yields( nextcf, &vals_returned, &labels);
  iterator_build_jtable( nextcf, cursor, &vals_returned, &labels);
  cleanup( ic_def->sym);
  normalize( ic_def);
  iterator_constructor_fixup( ic);
  ic->isIterator = true;
  nextcf->retType = dtInt[INT_SIZE_32];

  FnSymbol *headcf = new FnSymbol( "getHeadCursor");
  iterator_method( headcf);
  m->stmts->insertAtHead(new DefExpr(headcf));
  iterator_formals( headcf, ic);
  headcf->body->insertAtHead( new ReturnStmt( new CallExpr( new CallExpr( ".", headcf->_this, new_StringSymbol( "getNextCursor")), new_IntSymbol(0))));
  headcf->retType = dtInt[INT_SIZE_32];

  FnSymbol *elemtf = new FnSymbol( "getElemType");
  iterator_method( elemtf);
  m->stmts->insertAtHead(new DefExpr(elemtf));
  iterator_formals( elemtf, ic);
  elemtf->body->insertAtHead( new ReturnStmt( new CallExpr( PRIMITIVE_TYPEOF, new CallExpr( new CallExpr( ".", elemtf->_this, new_StringSymbol( "getValue")), new_IntSymbol(0)))));

  FnSymbol *valuef = new FnSymbol( "getValue");
  iterator_method( valuef);
  valuef->retExprType = nextcf->retExprType->remove();
  m->stmts->insertAtHead(new DefExpr(valuef));
  iterator_formals( valuef, ic, cursor);
  iterator_build_vtable( valuef, cursor, &vals_returned);
  iterator_update_this_uses(valuef, 
                            valuef->getFormal(2)->defPoint, 
                            nextcf->getFormal(2)->defPoint);

  FnSymbol *isvalidcf = new FnSymbol( "isValidCursor?");
  iterator_method( isvalidcf);
  m->stmts->insertAtHead(new DefExpr(isvalidcf));
  isvalidcf->body->insertAtHead( new ReturnStmt( new CallExpr( "!=", cursor, new_IntSymbol(vals_returned.length()+1))));
  iterator_formals( isvalidcf, ic, cursor);

  // iterator -> wrapper function
  fn->fnClass = FN_FUNCTION;
  fn->retType = dtUnknown;
  fn->retExprType->remove();
  CallExpr* wrapperCall = new CallExpr(ic->defaultConstructor);
  for_formals(formal, fn) {
    if (formal->type != dtMethodToken)
      wrapperCall->insertAtTail(formal);
  }
  fn->body->replace(new BlockStmt(new ReturnStmt(wrapperCall)));
  fn->retExprType = wrapperCall->copy();
  insert_help(fn->retExprType, NULL, fn, fn->argScope);
  normalize(fn->defPoint);
}


static void
enable_scalar_promotion( FnSymbol *fn) {
  Expr* seqType = fn->retExprType;
  if (!seqType)
    return;
  Type *seqElementType = seqType->typeInfo();
  
  if (!strcmp("_promoter", fn->name)) {
    if (seqElementType != dtUnknown) {
      fn->_this->type->scalarPromotionType = seqElementType;
    } else {
      if (CallExpr *c = dynamic_cast<CallExpr*>(seqType)) {
        if (SymExpr *b = dynamic_cast<SymExpr*>(c->baseExpr)) {
          if (!strcmp(".", b->var->name) && c->argList->length() == 2) {
            if (SymExpr *a1 = dynamic_cast<SymExpr*>(c->argList->get(1))) {
              if (a1->var == fn->_this) {
                if (SymExpr *a2 = dynamic_cast<SymExpr*>(c->argList->get(2))) {
                  if (VarSymbol *vs = dynamic_cast<VarSymbol*>(a2->var)) {
                    if (vs->immediate) {
                      char *s = vs->immediate->v_string;
                      ClassType *ct = dynamic_cast<ClassType*>(fn->_this->type);
                      for_fields(field, ct)
                        if (!strcmp(field->name, s))
                          field->addPragma("promoter");
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}


static void build_lvalue_function(FnSymbol* fn) {
  FnSymbol* new_fn = fn->copy();
  fn->defPoint->insertAfter(new DefExpr(new_fn));
  if (fn->_this)
    fn->_this->type->methods.add(new_fn);
  fn->buildSetter = false;
  new_fn->retType = dtVoid;
  new_fn->cname = stringcat("_setter_", fn->cname);
  ArgSymbol* setterToken = new ArgSymbol(INTENT_BLANK, "_st",
                                         dtSetterToken);
  ArgSymbol* lvalue = new ArgSymbol(INTENT_BLANK, "_lvalue", dtAny);
  Expr* exprType = NULL;
  if (new_fn->retExprType) {
    lvalue->type = dtUnknown;
    exprType = new_fn->retExprType;
    exprType->remove();
  }
  new_fn->insertFormalAtTail(new DefExpr(setterToken));
  new_fn->insertFormalAtTail(new DefExpr(lvalue, NULL, exprType));
  Vec<BaseAST*> asts;
  collect_asts_postorder(&asts, new_fn->body);
  forv_Vec(BaseAST, ast, asts) {
    if (ReturnStmt* returnStmt = dynamic_cast<ReturnStmt*>(ast)) {
      if (returnStmt->parentSymbol == new_fn) {
        Expr* expr = returnStmt->expr;
        returnStmt->expr->replace(new SymExpr(gVoid));
        returnStmt->insertBefore(new CallExpr("=", expr, lvalue));
      }
    }
  }
}


static void normalize_returns(FnSymbol* fn) {
  Vec<BaseAST*> asts;
  Vec<ReturnStmt*> rets;
  collect_asts(&asts, fn);
  forv_Vec(BaseAST, ast, asts) {
    if (ReturnStmt* returnStmt = dynamic_cast<ReturnStmt*>(ast)) {
      if (returnStmt->parentSymbol == fn) // not in a nested function
        rets.add(returnStmt);
    }
  }
  if (rets.n == 0) {
    fn->insertAtTail(new ReturnStmt(gVoid));
    return;
  }
  if (rets.n == 1) {
    ReturnStmt* ret = rets.v[0];
    if (ret == fn->body->body->last() && dynamic_cast<SymExpr*>(ret->expr))
      return;
  }
  bool returns_void = rets.v[0]->returnsVoid();
  LabelSymbol* label = new LabelSymbol(stringcat("_end_", fn->name));
  fn->insertAtTail(new DefExpr(label));
  VarSymbol* retval = NULL;
  if (returns_void) {
    fn->insertAtTail(new ReturnStmt());
  } else {
    retval = new VarSymbol(stringcat("_ret_", fn->name), fn->retType);
    retval->isCompilerTemp = true;
    retval->canReference = true;
    if (fn->isParam)
      retval->consClass = VAR_PARAM;
    fn->insertAtHead(new DefExpr(retval));
    fn->insertAtTail(new ReturnStmt(retval));
  }
  bool label_is_used = false;
  forv_Vec(ReturnStmt, ret, rets) {
    if (retval) {
      Expr* ret_expr = ret->expr;
      ret_expr->remove();
      if (fn->retExprType)
        ret_expr = new CallExpr("_cast", fn->retExprType->copy(), ret_expr);
      ret->insertBefore(new CallExpr(PRIMITIVE_MOVE, retval, ret_expr));
    }
    if (ret->next != label->defPoint) {
      ret->replace(new GotoStmt(goto_normal, label));
      label_is_used = true;
    } else {
      ret->remove();
    }
  }
  if (!label_is_used)
    label->defPoint->remove();
}


static void call_constructor_for_class(CallExpr* call) {
  if (SymExpr* baseVar = dynamic_cast<SymExpr*>(call->baseExpr)) {
    if (TypeSymbol* ts = dynamic_cast<TypeSymbol*>(baseVar->var)) {
      if (ClassType* ct = dynamic_cast<ClassType*>(ts->type)) {
        if (ct->defaultConstructor)
          call->baseExpr->replace(new SymExpr(ct->defaultConstructor->name));
        else
          INT_FATAL(call, "class type has no default constructor");
      }
    }
  }
}


static void decompose_special_calls(CallExpr* call) {
  if (call->isResolved())
    return;
  if (!call->argList->isEmpty() > 0) {
    Expr* firstArg = dynamic_cast<Expr*>(call->argList->get(1));
    SymExpr* symArg = dynamic_cast<SymExpr*>(firstArg);
    // don't decompose method calls
    if (symArg && symArg->var == gMethodToken)
      return;
  }
  if (call->isNamed(".")) {
    if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(2))) {
      if (VarSymbol* var = dynamic_cast<VarSymbol*>(sym->var)) {
        if (var->immediate &&
            var->immediate->const_kind == CONST_KIND_STRING &&
            !strcmp(var->immediate->v_string, "read")) {
          if (CallExpr* parent = dynamic_cast<CallExpr*>(call->parentExpr)) {
            while (parent->argList->length() > 1) {
              Expr* arg = parent->get(1)->remove();
              parent->getStmtExpr()->insertBefore(new CallExpr(call->copy(), arg));
            }
          }
        }
      }
    }
  } else if (call->isNamed("read")) {
    for_actuals(actual, call) {
      actual->remove();
      call->getStmtExpr()->insertBefore(
        new CallExpr(
          new CallExpr(".", chpl_stdin, new_StringSymbol("read")), actual));
    }
    call->getStmtExpr()->remove();
  }
}


static void apply_getters_setters(FnSymbol* fn) {
  // Most generally:
  //   x.f(a) = y --> f(_mt, x)(a, _st, y)
  // which is the same as
  //   call(= call(call(. x "f") a) y) --> call(call(f _mt x) a _st y)
  // Also:
  //   x.f = y --> f(_mt, x, _st, y)
  //   f(a) = y --> f(a, _st, y)
  //   x.f --> f(_mt, x)
  //   x.f(a) --> f(_mt, x)(a)
  // Note:
  //   call(call or )( indicates partial
  Vec<BaseAST*> asts;
  collect_asts_postorder(&asts, fn);
  forv_Vec(BaseAST, ast, asts) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(ast)) {
      currentLineno = call->lineno;
      currentFilename = call->filename;
      if (call->getFunction() != fn) // in a nested function, handle
                                     // later, because it may be a
                                     // getter or a setter
        continue;
      if (call->isNamed(".")) { // handle getter
        if (CallExpr* parent = dynamic_cast<CallExpr*>(call->parentExpr))
          if (parent->isNamed("="))
            if (parent->get(1) == call)
              continue; // handle setter below
        char* method = NULL;
        if (SymExpr* symExpr = dynamic_cast<SymExpr*>(call->get(2)))
          if (VarSymbol* var = dynamic_cast<VarSymbol*>(symExpr->var))
            if (var->immediate->const_kind == CONST_KIND_STRING)
              method = var->immediate->v_string;
        if (!method)
          INT_FATAL(call, "No method name for getter or setter");
        Expr* _this = call->get(1);
        _this->remove();
        CallExpr* getter = new CallExpr(method, gMethodToken, _this);
        getter->methodTag = true;
        call->replace(getter);
        if (CallExpr* parent = dynamic_cast<CallExpr*>(getter->parentExpr))
          if (parent->baseExpr == getter)
            getter->partialTag = true;
      } else if (call->isNamed("=")) {
        if (CallExpr* lhs = dynamic_cast<CallExpr*>(call->get(1))) {
          if (lhs->isNamed(".")) {
            char* method = NULL;
            if (SymExpr* symExpr = dynamic_cast<SymExpr*>(lhs->get(2)))
              if (VarSymbol* var = dynamic_cast<VarSymbol*>(symExpr->var))
                if (var->immediate->const_kind == CONST_KIND_STRING)
                  method = var->immediate->v_string;
            if (!method)
              INT_FATAL(call, "No method name for getter or setter");
            Expr* _this = lhs->get(1);
            _this->remove();
            Expr* rhs = call->get(2);
            rhs->remove();
            CallExpr* setter =
              new CallExpr(method, gMethodToken, _this, gSetterToken, rhs);
            call->replace(setter);
          } else {
            Expr* rhs = call->get(2);
            rhs->remove();
            lhs->remove();
            call->replace(lhs);
            lhs->insertAtTail(gSetterToken);
            lhs->insertAtTail(rhs);
          }
        }
      }
    }
  }
}


static void insert_call_temps(CallExpr* call) {
  if (!call->parentExpr || !call->getStmtExpr())
    return;

  if (call == call->getStmtExpr())
    return;
  
  if (dynamic_cast<DefExpr*>(call->parentExpr))
    return;

  if (call->partialTag)
    return;

  if (call->primitive)
    return;

  if (CallExpr* parentCall = dynamic_cast<CallExpr*>(call->parentExpr)) {
    if (parentCall->isPrimitive(PRIMITIVE_MOVE) ||
        parentCall->isPrimitive(PRIMITIVE_REF))
      return;
    if (parentCall->isNamed("_init"))
      call = parentCall;
  }

  Expr* stmt = call->getStmtExpr();
  VarSymbol* tmp = new VarSymbol("_tmp", dtUnknown, VAR_NORMAL, VAR_CONST);
  tmp->isCompilerTemp = true;
  tmp->canReference = true;
  tmp->canParam = true;
  tmp->canType = true;
  call->replace(new SymExpr(tmp));
  stmt->insertBefore(new DefExpr(tmp));
  stmt->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, call));
}


static void fix_user_assign(CallExpr* call) {
  if (!call->parentExpr ||
      call->getStmtExpr() == call->parentExpr ||
      !call->isNamed("="))
    return;
  CallExpr* move = new CallExpr(PRIMITIVE_MOVE, call->get(1)->copy());
  call->replace(move);
  move->insertAtTail(call);
}

//
// fix_def_expr removes DefExpr::exprType and DefExpr::init from a
//   variable's def expression, normalizing the AST with primitive
//   moves, calls to _copy, _init, and _cast, and assignments.
//
static void
fix_def_expr(VarSymbol* var) {
  Expr* type = var->defPoint->exprType;
  Expr* init = var->defPoint->init;
  Expr* stmt = var->defPoint; // insertion point
  VarSymbol* constTemp = var; // temp for constants

  if (!type && !init)
    return; // already fixed

  //
  // insert temporary for constants to assist constant checking
  //
  if (var->consClass == VAR_CONST) {
    constTemp = new VarSymbol("_constTmp");
    constTemp->isCompilerTemp = true;
    constTemp->canReference = true;
    stmt->insertBefore(new DefExpr(constTemp));
    stmt->insertAfter(new CallExpr(PRIMITIVE_MOVE, var, constTemp));
  }

  //
  // insert code to initialize config variable from the command line
  //
  if (var->varClass == VAR_CONFIG) {
    Expr* noop = new CallExpr(PRIMITIVE_NOOP);
    ModuleSymbol* module = var->getModule();
    stmt->insertAfter(
      new CondStmt(
        new CallExpr("!",
          new CallExpr(primitives_map.get("_config_has_value"),
                       new_StringSymbol(var->name),
                       new_StringSymbol(module->name))),
        noop,
        new CallExpr(PRIMITIVE_MOVE, constTemp,
          new CallExpr("_cast",
            new CallExpr(PRIMITIVE_TYPEOF, constTemp),
            new CallExpr(primitives_map.get("_config_get_value"),
                         new_StringSymbol(var->name),
                         new_StringSymbol(module->name))))));
    stmt = noop; // insert regular definition code in then block
  }

  if (type) {

    //
    // use cast for parameters to avoid multiple parameter assignments
    //
    if (init && var->consClass == VAR_PARAM) {
      stmt->insertAfter(
        new CallExpr(PRIMITIVE_MOVE, var,
          new CallExpr("_cast", type->remove(), init->remove())));
      return;
    }

    //
    // initialize variable based on specified type and then assign it
    // the initialization expression if it exists
    //
    VarSymbol* typeTemp = new VarSymbol("_typeTmp");
    typeTemp->isTypeVariable = true;
    stmt->insertBefore(new DefExpr(typeTemp));
    stmt->insertBefore(
      new CallExpr(PRIMITIVE_MOVE, typeTemp,
        new CallExpr("_init", type->remove())));
    if (init)
      stmt->insertAfter(
        new CallExpr(PRIMITIVE_MOVE, constTemp,
          new CallExpr("=", constTemp, init->remove())));
    stmt->insertAfter(new CallExpr(PRIMITIVE_MOVE, constTemp, typeTemp));

  } else {

    //
    // initialize untyped variable with initialization expression
    //
    stmt->insertAfter(
      new CallExpr(PRIMITIVE_MOVE, constTemp,
        new CallExpr("_copy", init->remove())));

  }
}


static void hack_resolve_types(Expr* expr) {
  if (DefExpr* def = dynamic_cast<DefExpr*>(expr)) {
    if (ArgSymbol* arg = dynamic_cast<ArgSymbol*>(def->sym)) {
      if (arg->type == dtUnknown && def->exprType) {
        Type* type = def->exprType->typeInfo();
        if (type != dtUnknown && type != dtAny) {
          arg->type = type;
          def->exprType->remove();
        }
      }
    }
  }
}


static void tag_global(FnSymbol* fn) {
  if (fn->global)
    return;
  for_formals(formal, fn) {
    if (ClassType* ct = dynamic_cast<ClassType*>(formal->type))
      if (ct->classTag == CLASS_CLASS &&
          !ct->symbol->hasPragma("domain") &&
          !ct->symbol->hasPragma("array"))
        fn->global = true;
    if (SymExpr* sym = dynamic_cast<SymExpr*>(formal->defPoint->exprType))
      if (ClassType* ct = dynamic_cast<ClassType*>(sym->var->type))
        if (ct->classTag == CLASS_CLASS &&
            !ct->symbol->hasPragma("domain") &&
            !ct->symbol->hasPragma("array"))
          fn->global = true;
  }
  if (fn->global) {
    fn->parentScope->removeVisibleFunction(fn);
    rootScope->addVisibleFunction(fn);
    if (dynamic_cast<FnSymbol*>(fn->defPoint->parentSymbol)) {
      ModuleSymbol* mod = fn->getModule();
      Expr* def = fn->defPoint;
      CallExpr* noop = new CallExpr(PRIMITIVE_NOOP);
      def->insertBefore(noop);
      fn->visiblePoint = noop;
      def->remove();
      mod->stmts->insertAtTail(def);
    }
  }
}


static void fixup_array_formals(FnSymbol* fn) {
  if (fn->normalizedOnce)
    return;
  fn->normalizedOnce = true;
  Vec<BaseAST*> asts;
  collect_asts(&asts, fn);
  forv_Vec(BaseAST, ast, asts) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(ast)) {
      if (call->isNamed("_build_array_type")) {
        SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1));
        DefExpr* def = dynamic_cast<DefExpr*>(call->get(1));
        DefExpr* parent = dynamic_cast<DefExpr*>(call->parentExpr);
        if (call->argList->length() == 1)
          if (!parent || !dynamic_cast<ArgSymbol*>(parent->sym) ||
              parent->exprType != call)
            USR_FATAL(call, "array without element type can only "
                      "be used as a formal argument type");
        if (def || (sym && sym->var == gNil) || call->argList->length() == 1) {
          if (!parent || !dynamic_cast<ArgSymbol*>(parent->sym)
              || parent->exprType != call)
            USR_FATAL(call, "array with empty or queried domain can "
                      "only be used as a formal argument type");
          parent->exprType->replace(new SymExpr(chpl_array));
          if (!fn->where) {
            fn->where = new BlockStmt(new SymExpr(gTrue));
            insert_help(fn->where, NULL, fn, fn->argScope);
          }
          Expr* expr = fn->where->body->only();
          if (call->argList->length() == 2)
            expr->replace(new CallExpr("&", expr->copy(),
                            new CallExpr("==", call->get(2)->remove(),
                              new CallExpr(".", parent->sym, new_StringSymbol("elt_type")))));
          if (def) {
            forv_Vec(BaseAST, ast, asts) {
              if (SymExpr* sym = dynamic_cast<SymExpr*>(ast)) {
                if (sym->var == def->sym)
                  sym->replace(new CallExpr(".", parent->sym, new_StringSymbol("dom")));
              }
            }
          } else if (!sym || sym->var != gNil) {
            VarSymbol* tmp = new VarSymbol(stringcat("_view_", parent->sym->name));
            forv_Vec(BaseAST, ast, asts) {
              if (SymExpr* sym = dynamic_cast<SymExpr*>(ast)) {
                if (sym->var == parent->sym)
                  sym->var = tmp;
              }
            }
            fn->insertAtHead(new CondStmt(
                               new SymExpr(parent->sym),
                                 new CallExpr(PRIMITIVE_MOVE, tmp,
                                   new CallExpr(new CallExpr(".", parent->sym,
                                     new_StringSymbol("view")),
                                     call->get(1)->copy()))));
            fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, parent->sym));
            fn->insertAtHead(new DefExpr(tmp));
          }
        } else {  //// DUPLICATED CODE ABOVE AND BELOW
          DefExpr* parent = dynamic_cast<DefExpr*>(call->parentExpr);
          if (parent && dynamic_cast<ArgSymbol*>(parent->sym) && parent->exprType == call) {
            VarSymbol* tmp = new VarSymbol(stringcat("_view_", parent->sym->name));
            forv_Vec(BaseAST, ast, asts) {
              if (SymExpr* sym = dynamic_cast<SymExpr*>(ast)) {
                if (sym->var == parent->sym)
                  sym->var = tmp;
              }
            }
            fn->insertAtHead(new CondStmt(
                               new SymExpr(parent->sym),
                                 new CallExpr(PRIMITIVE_MOVE, tmp,
                                   new CallExpr(new CallExpr(".", parent->sym,
                                     new_StringSymbol("view")),
                                     call->get(1)->copy()))));
            fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, parent->sym));
            fn->insertAtHead(new DefExpr(tmp));
          }
        }
      }
    }
  }
}


static void clone_parameterized_primitive_methods(FnSymbol* fn) {
  if (dynamic_cast<ArgSymbol*>(fn->_this)) {
    if (fn->_this->type == dtInt[INT_SIZE_32]) {
      for (int i=INT_SIZE_1; i<INT_SIZE_NUM; i++) {
        if (dtInt[i] && i != INT_SIZE_32) {
          FnSymbol* nfn = fn->copy();
          nfn->_this->type = dtInt[i];
          fn->defPoint->insertBefore(new DefExpr(nfn));
        }
      }
    }
    if (fn->_this->type == dtUInt[INT_SIZE_32]) {
      for (int i=INT_SIZE_1; i<INT_SIZE_NUM; i++) {
        if (dtUInt[i] && i != INT_SIZE_32) {
          FnSymbol* nfn = fn->copy();
          nfn->_this->type = dtUInt[i];
          fn->defPoint->insertBefore(new DefExpr(nfn));
        }
      }
    }
    if (fn->_this->type == dtReal[FLOAT_SIZE_64]) {
      for (int i=FLOAT_SIZE_16; i<FLOAT_SIZE_NUM; i++) {
        if (dtReal[i] && i != FLOAT_SIZE_64) {
          FnSymbol* nfn = fn->copy();
          nfn->_this->type = dtReal[i];
          fn->defPoint->insertBefore(new DefExpr(nfn));
        }
      }
    }
    if (fn->_this->type == dtImag[FLOAT_SIZE_64]) {
      for (int i=FLOAT_SIZE_16; i<FLOAT_SIZE_NUM; i++) {
        if (dtImag[i] && i != FLOAT_SIZE_64) {
          FnSymbol* nfn = fn->copy();
          nfn->_this->type = dtImag[i];
          fn->defPoint->insertBefore(new DefExpr(nfn));
        }
      }
    }
    if (fn->_this->type == dtComplex[COMPLEX_SIZE_128]) {
      for (int i=COMPLEX_SIZE_32; i<COMPLEX_SIZE_NUM; i++) {
        if (dtComplex[i] && i != COMPLEX_SIZE_128) {
          FnSymbol* nfn = fn->copy();
          nfn->_this->type = dtComplex[i];
          fn->defPoint->insertBefore(new DefExpr(nfn));
        }
      }
    }
  }
}


static void
clone_for_parameterized_primitive_formals(FnSymbol* fn,
                                          DefExpr* def,
                                          int width) {
  compute_sym_uses(fn);
  ASTMap map;
  FnSymbol* newfn = fn->copy(&map);
  DefExpr* newdef = dynamic_cast<DefExpr*>(map.get(def));
  newdef->replace(new SymExpr(new_IntSymbol(width)));
  forv_Vec(SymExpr, sym, def->sym->uses) {
    SymExpr* newsym = dynamic_cast<SymExpr*>(map.get(sym));
    newsym->var = new_IntSymbol(width);
  }
  fn->defPoint->insertAfter(new DefExpr(newfn));
  fixup_parameterized_primitive_formals(newfn);
}

static void
fixup_parameterized_primitive_formals(FnSymbol* fn) {
  Vec<BaseAST*> asts;
  collect_top_asts(&asts, fn);
  forv_Vec(BaseAST, ast, asts) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(ast)) {
      if (call->argList->length() != 1)
        continue;
      if (DefExpr* def = dynamic_cast<DefExpr*>(call->get(1))) {
        if (call->isNamed("int") || call->isNamed("uint")) {
          for( int i=INT_SIZE_1; i<INT_SIZE_NUM; i++)
            if (dtInt[i])
              clone_for_parameterized_primitive_formals(fn, def,
                                                        get_width(dtInt[i]));
          fn->defPoint->remove();
        } else if (call->isNamed("real") || call->isNamed("imag")) {
          for( int i=FLOAT_SIZE_16; i<FLOAT_SIZE_NUM; i++)
            if (dtReal[i])
              clone_for_parameterized_primitive_formals(fn, def,
                                                        get_width(dtReal[i]));
          fn->defPoint->remove();
        } else if (call->isNamed("complex")) {
          for( int i=COMPLEX_SIZE_32; i<COMPLEX_SIZE_NUM; i++)
            if (dtComplex[i])
              clone_for_parameterized_primitive_formals(fn, def,
                                                        get_width(dtComplex[i]));
          fn->defPoint->remove();
        }
      }
    }
  }
}


static void change_method_into_constructor(FnSymbol* fn) {
  if (fn->formals->length() <= 1)
    return;
  if (fn->getFormal(1)->type != dtMethodToken)
    return;
  if (fn->getFormal(2)->type == dtUnknown)
    INT_FATAL(fn, "this argument has unknown type");
  if (strcmp(fn->getFormal(2)->type->symbol->name, fn->name))
    return;
  ClassType* ct = dynamic_cast<ClassType*>(fn->getFormal(2)->type);
  if (!ct)
    INT_FATAL(fn, "constructor on non-class type");
  fn->name = canonicalize_string(stringcat("_construct_", fn->name));
  fn->_this = new VarSymbol("this", ct);
  fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, fn->_this, new CallExpr(ct->symbol)));
  fn->insertAtHead(new DefExpr(fn->_this));
  fn->insertAtTail(new ReturnStmt(new SymExpr(fn->_this)));
  ASTMap map;
  map.put(fn->getFormal(2), fn->_this);
  fn->formals->get(2)->remove();
  fn->formals->get(1)->remove();
  update_symbols(fn, &map);
  ct->symbol->defPoint->insertBefore(fn->defPoint->remove());
}
