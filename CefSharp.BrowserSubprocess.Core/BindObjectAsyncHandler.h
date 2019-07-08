// Copyright © 2019 The CefSharp Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#pragma once

#include "include/cef_v8.h"
#include "RegisterBoundObjectRegistry.h"
#include "..\CefSharp.Core\Internals\Messaging\Messages.h"
#include "..\CefSharp.Core\Internals\Serialization\Primitives.h"

using namespace System;
using namespace CefSharp::Internals::Messaging;
using namespace CefSharp::Internals::Serialization;

namespace CefSharp
{
    const CefString kBindObjectAsync = CefString("BindObjectAsync");
    const CefString kBindObjectAsyncCamelCase = CefString("bindObjectAsync");

    private class BindObjectAsyncHandler : public CefV8Handler
    {
    private:
        gcroot<RegisterBoundObjectRegistry^> _callbackRegistry;
        gcroot<Dictionary<String^, JavascriptObject^>^> _javascriptObjects;
        gcroot<CefBrowserWrapper^> _browserWrapper;

    public:
        BindObjectAsyncHandler(RegisterBoundObjectRegistry^ callbackRegistery, Dictionary<String^, JavascriptObject^>^ javascriptObjects, CefBrowserWrapper^ browserWrapper)
        {
            _callbackRegistry = callbackRegistery;
            _javascriptObjects = javascriptObjects;
            _browserWrapper = browserWrapper;
        }

        bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) OVERRIDE
        {
            auto context = CefV8Context::GetCurrentContext();
            if (context.get() && context->Enter())
            {
                try
                {
                    auto global = context->GetGlobal();

                    CefRefPtr<CefV8Value> promiseData;
                    CefRefPtr<CefV8Exception> promiseException;
                    //this will create a promise and give us the reject/resolve functions {p: Promise, res: resolve(), rej: reject()}
                    if (!context->Eval(CefAppUnmanagedWrapper::kPromiseCreatorScript, CefString(), 0, promiseData, promiseException))
                    {
                        exception = promiseException->GetMessage();

                        return true;
                    }

                    //return the promose
                    retval = promiseData->GetValue("p");

                    //References to the promise resolve and reject methods
                    auto resolve = promiseData->GetValue("res");
                    auto reject = promiseData->GetValue("rej");

                    auto callback = gcnew JavascriptAsyncMethodCallback(context, resolve, reject);

                    auto request = CefProcessMessage::Create(kJavascriptRootObjectRequest);
                    auto argList = request->GetArgumentList();
                    auto params = CefListValue::Create();

                    auto boundObjectRequired = false;
                    auto notifyIfAlreadyBound = false;
                    auto ignoreCache = false;
                    auto cachedObjects = gcnew List<JavascriptObject^>();
                    //TODO: Create object to represent this information
                    auto objectNamesWithBoundStatus = gcnew List<Tuple<String^, bool, bool>^>();
                    auto objectCount = 0;

                    if (arguments.size() > 0)
                    {
                        objectCount = (int)arguments.size();

                        //If first argument is an object, we'll see if it contains config values
                        if (arguments[0]->IsObject())
                        {
                            //Upper and camelcase options are supported
                            notifyIfAlreadyBound = GetV8BoolValue(arguments[0], "NotifyIfAlreadyBound", "notifyIfAlreadyBound");
                            ignoreCache = GetV8BoolValue(arguments[0], "IgnoreCache", "ignoreCache");

                            //If we have a config object then we remove that from the count
                            objectCount = objectCount - 1;
                        }

                        //Loop through all arguments and ignore anything that's not a string
                        for (auto i = 0; i < arguments.size(); i++)
                        {
                            //Validate arg as being a string
                            if (arguments[i]->IsString())
                            {
                                auto objectName = arguments[i]->GetStringValue();
                                auto managedObjectName = StringUtils::ToClr(objectName);
                                auto alreadyBound = global->HasValue(objectName);
                                auto cached = false;

                                //Check if the object has already been bound
                                if (alreadyBound)
                                {
                                    cached = _javascriptObjects->ContainsKey(managedObjectName);
                                }
                                else
                                {
                                    //If no matching object found then we'll add the object name to the list
                                    boundObjectRequired = true;
                                    params->SetString(i, objectName);

                                    JavascriptObject^ obj;
                                    if (_javascriptObjects->TryGetValue(managedObjectName, obj))
                                    {
                                        cachedObjects->Add(obj);

                                        cached = true;
                                    }
                                }

                                objectNamesWithBoundStatus->Add(Tuple::Create(managedObjectName, alreadyBound, cached));
                            }
                        }
                    }
                    else
                    {
                        //No objects names were specified so we default to makeing the request
                        boundObjectRequired = true;
                    }

                    if (boundObjectRequired || ignoreCache)
                    {
                        //If the number of cached objects matches the number of args
                        //(we have a cached copy of all requested objects)
                        //then we'll immediately bind the cached objects
                        if (cachedObjects->Count == objectCount && ignoreCache == false)
                        {
                            auto frame = context->GetFrame();

                            if (frame.get() && frame->IsValid())
                            {
                                if (Object::ReferenceEquals(_browserWrapper, nullptr))
                                {
                                    callback->Fail("Browser wrapper is null and unable to bind objects");

                                    return true;
                                }

                                auto browser = context->GetBrowser();

                                auto rootObjectWrappers = _browserWrapper->JavascriptRootObjectWrappers;

                                JavascriptRootObjectWrapper^ rootObject;
                                if (!rootObjectWrappers->TryGetValue(frame->GetIdentifier(), rootObject))
                                {
                                    rootObject = gcnew JavascriptRootObjectWrapper(browser->GetIdentifier(), _browserWrapper->BrowserProcess);
                                    rootObjectWrappers->TryAdd(frame->GetIdentifier(), rootObject);
                                }

                                //Cached objects only contains a list of objects not already bound
                                rootObject->Bind(cachedObjects, context->GetGlobal());

                                //Response object has no Accessor or Interceptor
                                auto response = CefV8Value::CreateObject(NULL, NULL);

                                response->SetValue("Count", CefV8Value::CreateInt(cachedObjects->Count), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);
                                response->SetValue("Success", CefV8Value::CreateBool(true), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);
                                response->SetValue("Message", CefV8Value::CreateString("OK"), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);
                                callback->Success(response);

                                NotifyObjectBound(frame, objectNamesWithBoundStatus);
                            }

                        }
                        else
                        {
                            auto frame = context->GetFrame();
                            if (frame.get() && frame->IsValid())
                            {
                                //Obtain a callbackId then send off the Request for objects
                                auto callbackId = _callbackRegistry->SaveMethodCallback(callback);

                                SetInt64(argList, 0, callbackId);
                                argList->SetList(1, params);

                                frame->SendProcessMessage(CefProcessId::PID_BROWSER, request);
                            }
                        }
                    }
                    else
                    {
                        auto frame = context->GetFrame();

                        if (frame.get() && frame->IsValid())
                        {
                            //Objects already bound or ignore cache

                            //Response object has no Accessor or Interceptor
                            auto response = CefV8Value::CreateObject(NULL, NULL);

                            //Objects already bound so we immediately resolve the Promise
                            response->SetValue("Success", CefV8Value::CreateBool(false), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);
                            response->SetValue("Count", CefV8Value::CreateInt(0), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);
                            response->SetValue("Message", CefV8Value::CreateString("Object(s) already bound"), CefV8Value::PropertyAttribute::V8_PROPERTY_ATTRIBUTE_READONLY);

                            CefV8ValueList returnArgs;
                            returnArgs.push_back(response);
                            //If all the requested objects are bound then we immediately execute resolve
                            //with Success true and Count of 0
                            resolve->ExecuteFunctionWithContext(context, nullptr, returnArgs);

                            if (notifyIfAlreadyBound)
                            {
                                NotifyObjectBound(frame, objectNamesWithBoundStatus);
                            }
                        }
                    }
                }
                finally
                {
                    context->Exit();
                }
            }
            else
            {
                exception = "BindObjectAsyncHandler::Execute - Unable to Get or Enter Context";
            }


            return true;
        }

    private:
        void NotifyObjectBound(const CefRefPtr<CefFrame> frame, List<Tuple<String^, bool, bool>^>^ objectNamesWithBoundStatus)
        {
            //Send message notifying Browser Process of which objects were bound
            //We do this after the objects have been created in the V8Context to gurantee
            //they are accessible.
            auto msg = CefProcessMessage::Create(kJavascriptObjectsBoundInJavascript);
            auto args = msg->GetArgumentList();

            auto boundObjects = CefListValue::Create();
            auto index = 0;

            for each(auto obj in objectNamesWithBoundStatus)
            {
                auto dict = CefDictionaryValue::Create();

                auto name = obj->Item1;
                auto alreadyBound = obj->Item2;
                auto isCached = obj->Item3;
                dict->SetString("Name", StringUtils::ToNative(name));
                dict->SetBool("IsCached", isCached);
                dict->SetBool("AlreadyBound", alreadyBound);

                boundObjects->SetDictionary(index++, dict);
            }

            args->SetList(0, boundObjects);

            frame->SendProcessMessage(CefProcessId::PID_BROWSER, msg);
        }

        bool GetV8BoolValue(const CefRefPtr<CefV8Value> val, const CefString key, const CefString camelCaseKey)
        {
            if (val->HasValue(key))
            {
                auto obj = val->GetValue(key);
                if (obj->IsBool())
                {
                    return obj->GetBoolValue();
                }
            }

            if (val->HasValue(camelCaseKey))
            {
                auto obj = val->GetValue(camelCaseKey);
                if (obj->IsBool())
                {
                    return obj->GetBoolValue();
                }
            }

            return false;
        }


        IMPLEMENT_REFCOUNTING(BindObjectAsyncHandler);
    };
}

