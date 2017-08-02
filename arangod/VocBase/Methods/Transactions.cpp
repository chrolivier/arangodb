#include "Transactions.h"
#include <v8.h>

#include "Transaction/Options.h"
#include "Transaction/UserTransaction.h"
#include "Transaction/V8Context.h"
#include "V8/v8-conv.h"
#include "V8/v8-vpack.h"
#include "V8/v8-helper.h"
#include "V8Server/V8Context.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "Logger/Logger.h"
#include <velocypack/Slice.h>

namespace arangodb {

class v8gHelper {
  // raii helper
  TRI_v8_global_t* _v8g;
  v8::Isolate* _isolate;

public:
  v8gHelper(v8::Isolate* isolate
           ,v8::Handle<v8::Value>& request)
           : _isolate(isolate)
  {
    TRI_GET_GLOBALS();
    _v8g = v8g;
    _v8g->_currentRequest = request;
  }

  ~v8gHelper() {
    _v8g->_currentRequest = v8::Undefined(_isolate);
  }
};

Result executeTransaction(TRI_vocbase_t* database, VPackSlice slice, std::string portType, VPackBuilder& builder){

  Result rv;

  if(!slice.isObject()){
    rv.reset(TRI_ERROR_BAD_PARAMETER, "body is not an object");
    return rv;
  }

  V8Context* context = V8DealerFeature::DEALER->enterContext(database, true);
  if (!context){
    rv.reset(TRI_ERROR_INTERNAL, "unable to get v8 context");
    return rv;
  }
  TRI_DEFER(V8DealerFeature::DEALER->exitContext(context));

  {
    v8::Isolate* isolate = context->_isolate;
    v8::HandleScope scope(isolate);
    v8::Handle<v8::Value> in = TRI_VPackToV8(isolate, slice);

    v8::Handle<v8::Value> result;
    v8::TryCatch tryCatch;
    bool canContinue = true;

    TRI_GET_GLOBALS();
    v8::Handle<v8::Object> request = v8::Object::New(isolate);
    v8::Handle<v8::Value> jsPortTypeKey= TRI_V8_ASCII_STRING("portType");
    v8::Handle<v8::Value> jsPortTypeValue = TRI_V8_ASCII_STRING(portType.c_str());
    if (!request->Set(jsPortTypeKey, jsPortTypeValue)){
      rv.reset(TRI_ERROR_INTERNAL, "could not set porttype");
      return rv;
    }
    {
      auto requestVal = v8::Handle<v8::Value>::Cast(request);
      v8gHelper globalVars(isolate, requestVal);
      rv = executeTransactionJS(context->_isolate, in, result, tryCatch, canContinue);
    }
    if(!canContinue) {
      //we need to cancel the context
      v8g->_canceled = true;
    }

    if (tryCatch.HasCaught()){
      //we have some javascript error that is not an arangoError
      std::string msg = *v8::String::Utf8Value(tryCatch.Message()->Get());
      rv.reset(TRI_ERROR_INTERNAL, msg);
    }

    if(rv.fail()){
      return rv;
    };

    if(result->IsUndefined()){
      // turn undefined to none
      builder.add(VPackSlice::noneSlice());
    } else {
      TRI_V8ToVPack(context->_isolate, builder, result, false);
    }

  }
  return rv;
}

// could go into basic lib
std::tuple<bool,bool,Result> extractArangoError(v8::Isolate* isolate, v8::TryCatch& tryCatch){
  // function tries to receive arango error form tryCatch Object
  // return tuple:
	//   bool - can continue
  //   bool - could convert
  //   result - extracted arango error
  std::tuple<bool,bool,Result> rv = {};
  std::get<0>(rv) = tryCatch.CanContinue();
  std::get<1>(rv) = false;
  v8::Handle<v8::Value> exception = tryCatch.Exception();
  if(!exception->IsObject()){
    return rv;
  }

  v8::Handle<v8::Object> object = v8::Handle<v8::Object>::Cast(exception);

  try {

    if(object->Has(TRI_V8_ASCII_STRING("errorNum")) &&
       object->Has(TRI_V8_ASCII_STRING("errorMessage"))
      )
    {
      std::uint64_t errorNum = TRI_ObjectToInt64(object->Get(TRI_V8_ASCII_STRING("errorNum")));
      std::string  errorMessage = *v8::String::Utf8Value(object->Get(TRI_V8_ASCII_STRING("errorMessage")));
      std::get<1>(rv) = true;
      std::get<2>(rv).reset(errorNum,errorMessage);
      tryCatch.Reset();
      return rv;
    }

    if(object->Has(TRI_V8_ASCII_STRING("name")) &&
       object->Has(TRI_V8_ASCII_STRING("message"))
      )
    {
      std::string  name = *v8::String::Utf8Value(object->Get(TRI_V8_ASCII_STRING("name")));
      std::string  message = *v8::String::Utf8Value(object->Get(TRI_V8_ASCII_STRING("message")));
      if(name == "TypeError"){
        std::get<2>(rv).reset(TRI_ERROR_TYPE_ERROR, message);
      } else {
        std::get<2>(rv).reset(TRI_ERROR_INTERNAL, name + ": " + message);
      }
      std::get<1>(rv) = true;
      tryCatch.Reset();
      return rv;
    }
  } catch (...) {
    // fail to extract but do nothing about it
  }

  return rv;
}

Result executeTransactionJS(
    v8::Isolate* isolate,
    v8::Handle<v8::Value> const& arg,
    v8::Handle<v8::Value>& result,
    v8::TryCatch& tryCatch,
    bool& canContinue)
{

  canContinue=true;
  Result rv;

  TRI_vocbase_t* vocbase = GetContextVocBase(isolate);
  if (vocbase == nullptr) {
    rv.reset(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    return rv;
  }

  // treat the value as an object from now on
  v8::Handle<v8::Object> object = v8::Handle<v8::Object>::Cast(arg);

  VPackBuilder builder;
  TRI_V8ToVPack(isolate, builder, object, false);

  // extract the properties from the object
  transaction::Options trxOptions;

  if (object->Has(TRI_V8_ASCII_STRING("lockTimeout"))) {
    static std::string const timeoutError =
        "<lockTimeout> must be a valid numeric value";

    if (!object->Get(TRI_V8_ASCII_STRING("lockTimeout"))->IsNumber()) {
      rv.reset(TRI_ERROR_BAD_PARAMETER, timeoutError);
    }

    trxOptions.lockTimeout =
        TRI_ObjectToDouble(object->Get(TRI_V8_ASCII_STRING("lockTimeout")));

    if (trxOptions.lockTimeout < 0.0) {
      rv.reset(TRI_ERROR_BAD_PARAMETER, timeoutError);
    }

    if(rv.fail()){
      return rv;
    }
  }

  // "waitForSync"
  TRI_GET_GLOBALS();
  TRI_GET_GLOBAL_STRING(WaitForSyncKey);
  if (object->Has(WaitForSyncKey)) {
    if (!object->Get(WaitForSyncKey)->IsBoolean() &&
        !object->Get(WaitForSyncKey)->IsBooleanObject()) {
      rv.reset(TRI_ERROR_BAD_PARAMETER, "<waitForSync> must be a boolean value");
      return rv;
    }

    trxOptions.waitForSync = TRI_ObjectToBoolean(WaitForSyncKey);
  }

  // "collections"
  std::string collectionError;

  if (!object->Has(TRI_V8_ASCII_STRING("collections")) ||
      !object->Get(TRI_V8_ASCII_STRING("collections"))->IsObject()) {
    collectionError = "missing/invalid collections definition for transaction";
    rv.reset(TRI_ERROR_BAD_PARAMETER, collectionError);
    return rv;
  }

  // extract collections
  v8::Handle<v8::Object> collections = v8::Handle<v8::Object>::Cast(
      object->Get(TRI_V8_ASCII_STRING("collections")));

  if (collections.IsEmpty()) {
    collectionError =
      "empty collections definition for transaction";
    rv.reset(TRI_ERROR_BAD_PARAMETER, collectionError);
    return rv;
  }

  std::vector<std::string> readCollections;
  std::vector<std::string> writeCollections;
  std::vector<std::string> exclusiveCollections;

  if (collections->Has(TRI_V8_ASCII_STRING("allowImplicit"))) {
    trxOptions.allowImplicitCollections = TRI_ObjectToBoolean(
        collections->Get(TRI_V8_ASCII_STRING("allowImplicit")));
  }

  if (object->Has(TRI_V8_ASCII_STRING("maxTransactionSize"))) {
    trxOptions.maxTransactionSize = TRI_ObjectToUInt64(
        object->Get(TRI_V8_ASCII_STRING("maxTransactionSize")), true);
  }
  if (object->Has(TRI_V8_ASCII_STRING("intermediateCommitSize"))) {
    trxOptions.intermediateCommitSize = TRI_ObjectToUInt64(
        object->Get(TRI_V8_ASCII_STRING("intermediateCommitSize")), true);
  }
  if (object->Has(TRI_V8_ASCII_STRING("intermediateCommitCount"))) {
    trxOptions.intermediateCommitCount = TRI_ObjectToUInt64(
        object->Get(TRI_V8_ASCII_STRING("intermediateCommitCount")), true);
  }

  auto getCollections = [&isolate](v8::Handle<v8::Object> obj,
                                   std::vector<std::string>& collections,
                                   char const* attributeName,
                                   std::string &collectionError) -> bool {
    if (obj->Has(TRI_V8_ASCII_STRING(attributeName))) {
      if (obj->Get(TRI_V8_ASCII_STRING(attributeName))->IsArray()) {
        v8::Handle<v8::Array> names = v8::Handle<v8::Array>::Cast(
            obj->Get(TRI_V8_ASCII_STRING(attributeName)));

        for (uint32_t i = 0; i < names->Length(); ++i) {
          v8::Handle<v8::Value> collection = names->Get(i);
          if (!collection->IsString()) {
            collectionError += std::string(" Collection name #") +
              std::to_string(i) + " in array '"+ attributeName +
              std::string("' is not a string");
            return false;
          }

          collections.emplace_back(TRI_ObjectToString(collection));
        }
      } else if (obj->Get(TRI_V8_ASCII_STRING(attributeName))->IsString()) {
        collections.emplace_back(
          TRI_ObjectToString(obj->Get(TRI_V8_ASCII_STRING(attributeName))));
      } else {
        collectionError += std::string(" There is no array in '") + attributeName + "'";
        return false;
      }
      // fallthrough intentional
    }
    return true;
  };

  collectionError = "invalid collection definition for transaction: ";
  // collections.read
  bool isValid =
    (getCollections(collections, readCollections, "read", collectionError) &&
     getCollections(collections, writeCollections, "write", collectionError) &&
     getCollections(collections, exclusiveCollections, "exclusive", collectionError));

  if (!isValid) {
    rv.reset(TRI_ERROR_BAD_PARAMETER, collectionError);
    return rv;
  }

  // extract the "action" property
  static std::string const actionErrorPrototype =
      "missing/invalid action definition for transaction";
  std::string actionError = actionErrorPrototype;

  if (!object->Has(TRI_V8_ASCII_STRING("action"))) {
    rv.reset(TRI_ERROR_BAD_PARAMETER, actionError);
    return rv;
  }

  // function parameters
  v8::Handle<v8::Value> params;

  if (object->Has(TRI_V8_ASCII_STRING("params"))) {
    params =
        v8::Handle<v8::Array>::Cast(object->Get(TRI_V8_ASCII_STRING("params")));
  } else {
    params = v8::Undefined(isolate);
  }

  if (params.IsEmpty()) {
    rv.reset(TRI_ERROR_BAD_PARAMETER, "unable to decode function parameters");
    return rv;
  }

  bool embed = false;
  if (object->Has(TRI_V8_ASCII_STRING("embed"))) {
    v8::Handle<v8::Value> v =
        v8::Handle<v8::Object>::Cast(object->Get(TRI_V8_ASCII_STRING("embed")));
    embed = TRI_ObjectToBoolean(v);
  }

  v8::Handle<v8::Object> current = isolate->GetCurrentContext()->Global();

  // callback function
  v8::Handle<v8::Function> action;
  if (object->Get(TRI_V8_ASCII_STRING("action"))->IsFunction()) {
    action = v8::Handle<v8::Function>::Cast(
        object->Get(TRI_V8_ASCII_STRING("action")));
    v8::Local<v8::Value> v8_fnname = action->GetName();
    std::string fnname = TRI_ObjectToString(v8_fnname);
    if (fnname.length() == 0) {
      action->SetName(TRI_V8_ASCII_STRING("userTransactionFunction"));
    }
  } else if (object->Get(TRI_V8_ASCII_STRING("action"))->IsString()) {
    // get built-in Function constructor (see ECMA-262 5th edition 15.3.2)
    v8::Local<v8::Function> ctor = v8::Local<v8::Function>::Cast(
        current->Get(TRI_V8_ASCII_STRING("Function")));

    // Invoke Function constructor to create function with the given body and no
    // arguments
    std::string body = TRI_ObjectToString( object->Get(TRI_V8_ASCII_STRING("action"))->ToString());
    body = "return (" + body + ")(params);";
    v8::Handle<v8::Value> args[2] = {TRI_V8_ASCII_STRING("params"), TRI_V8_STD_STRING(body)};
    v8::Local<v8::Object> function = ctor->NewInstance(2, args);

    action = v8::Local<v8::Function>::Cast(function);
    if (tryCatch.HasCaught()) {
      actionError += " - ";
      actionError += *v8::String::Utf8Value(tryCatch.Message()->Get());
      actionError += " - ";
      actionError += *v8::String::Utf8Value(tryCatch.StackTrace());
      rv.reset(TRI_ERROR_BAD_PARAMETER, actionError);
      tryCatch.Reset(); //reset as we have transferd the error message into the Result
      return rv;
    }
    action->SetName(TRI_V8_ASCII_STRING("userTransactionSource"));
  } else {
    rv.reset(TRI_ERROR_BAD_PARAMETER, actionError);
    return rv;
  }

  if (action.IsEmpty()) {
    rv.reset(TRI_ERROR_BAD_PARAMETER, actionError);
    return rv;
  }

  auto transactionContext =
      std::make_shared<transaction::V8Context>(vocbase, embed);

  // start actual transaction
  transaction::UserTransaction trx(transactionContext, readCollections, writeCollections, exclusiveCollections,
                          trxOptions);

  rv = trx.begin();
  if (rv.fail()) {
    return rv;
  }

  try {
    v8::Handle<v8::Value> arguments = params;
    result = action->Call(current, 1, &arguments);
    if (tryCatch.HasCaught()) {
      trx.abort();
      std::tuple<bool,bool,Result> rvTuple = extractArangoError(isolate, tryCatch);
      canContinue = std::get<0>(rvTuple);
      if (std::get<1>(rvTuple)){
        rv = std::get<2>(rvTuple);
      }
    }
  } catch (arangodb::basics::Exception const& ex) {
    rv.reset(ex.code(), ex.what());
  } catch (std::bad_alloc const&) {
    rv.reset(TRI_ERROR_OUT_OF_MEMORY);
  } catch (std::exception const& ex) {
    rv.reset(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    rv.reset(TRI_ERROR_INTERNAL, "caught unknown exception during transaction");
  }

  if (rv.fail()) {
    return rv;
  }

  rv = trx.commit();
  return rv;
}

} // arangodb
