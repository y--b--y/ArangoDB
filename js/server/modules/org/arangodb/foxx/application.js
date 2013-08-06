/*jslint indent: 2, nomen: true, maxlen: 120 */
/*global module, require, exports */

////////////////////////////////////////////////////////////////////////////////
/// @brief Foxx application
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2013 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Lucas Dohmen
/// @author Copyright 2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var Application,
  RequestContext = require("org/arangodb/foxx/request_context").RequestContext,
  db = require("org/arangodb").db,
  BaseMiddleware = require("org/arangodb/foxx/base_middleware").BaseMiddleware,
  extend = require("underscore").extend,
  is = require("org/arangodb/is"),
  internal = require("org/arangodb/foxx/internals");

// -----------------------------------------------------------------------------
// --SECTION--                                                       Application
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_initializer
/// @brief Create a new Application
///
/// @FUN{new FoxxApplication(@FA{applicationContext}, @FA{options})}
///
/// This creates a new Application. The first argument is the application
/// context available in the variable `applicationContext`. The second one is an
/// options array with the following attributes:
///
/// * `urlPrefix`: All routes you define within will be prefixed with it.
///
/// @EXAMPLES
///
/// @code
///     app = new Application(applicationContext, {
///       urlPrefix: "/meadow"
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

Application = function (context, options) {
  'use strict';
  var urlPrefix, baseMiddleware;

  if (is.notExisty(context)) {
    throw new Error("parameter <context> is missing");
  }

  this.routingInfo = {
    routes: []
  };

  options = options || {};
  urlPrefix = options.urlPrefix || "";

  if (urlPrefix === "") {
    urlPrefix = context.prefix;
  } else if (context.prefix !== "") {
    urlPrefix = context.prefix + "/" + urlPrefix;
  }

  this.routingInfo.urlPrefix = urlPrefix;

  baseMiddleware = new BaseMiddleware();

  this.routingInfo.middleware = [
    {
      url: { match: "/*" },
      action: {
        callback: baseMiddleware.stringRepresentation,
        options: {
          name: context.name,
          version: context.version,
          appId: context.appId,
          mount: context.mount,
          isDevelopment: context.isDevelopment,
          isProduction: context.isProduction,
          prefix: context.prefix,
          options: context.options
        }
      }
    }
  ];

  context.foxxes.push(this);

  this.applicationContext = context;
};

extend(Application.prototype, {

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_createRepository
/// @brief Create a repository
///
/// @FUN{FoxxApplication::createRepository(@FA{name}, @FA{options})}
///
/// A repository is a module that gets data from the database or saves data to
/// it. A model is a representation of data which will be used by the
/// repository. Use this method to create a repository and a corresponding
/// model.
///
/// @code
/// Foxx = require("org/arangodb/foxx");
///
/// app = new Foxx.Application(applicationContext);
///
/// var todos = app.createRepository("todos", {
///   model: "models/todos",
///   repository: "repositories/todos"
/// });
/// @endcode
///
/// If you do not give a repository, it will default to the
/// `Foxx.Repository`. If you need more than the methods provided by it, you
/// must give the path (relative to your lib directory) to your repository
/// module there. Then you can extend the Foxx.Repository prototype and add your
/// own methods.
///
/// If you don't need either of those, you don't need to give an empty
/// object. You can then just call:
///
/// @code
/// var todos = app.createRepository("todos");
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  createRepository: function (name, opts) {
    'use strict';
    var model,
      Repo,
      prefix,
      cname,
      collection,
      Model = require("org/arangodb/foxx/model").Model,
      Repository = require("org/arangodb/foxx/repository").Repository;

    if (opts && opts.hasOwnProperty('model')) {
      model = this.applicationContext.appModule.require(opts.model).Model;

      if (model === undefined) {
        throw new Error("module '" + opts.model + "' does not define a model");
      }
    } else {
      model = Model;
    }

    if (opts && opts.hasOwnProperty('repository')) {
      Repo = this.applicationContext.appModule.require(opts.repository).Repository;

      if (Repo === undefined) {
        throw new Error("module '" + opts.repository + "' does not define a repository");
      }
    } else {
      Repo = Repository;
    }

    prefix = this.applicationContext.collectionPrefix;

    if (prefix === "") {
      cname = name;
    } else {
      cname = prefix + "_" + name;
    }

    collection = db._collection(cname);

    if (!collection) {
      throw new Error("collection with name '" + cname + "' does not exist.");
    }
    return new Repo(prefix, collection, model);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_handleRequest
/// @brief Handle a request
///
/// The `handleRequest` method is the raw way to create a new route. You
/// probably wont call it directly, but it is used in the other request methods:
///
/// When defining a route you can also define a so called 'parameterized' route
/// like `/goose/:barn`. In this case you can later get the value the user
/// provided for `barn` via the `params` function (see the Request object).
////////////////////////////////////////////////////////////////////////////////

  handleRequest: function (method, route, callback) {
    'use strict';
    var newRoute = internal.constructRoute(method, route, callback);
    this.routingInfo.routes.push(newRoute);
    return new RequestContext(newRoute);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_head
/// @brief Handle a `head` request
///
/// @FUN{FoxxApplication::head(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `head`.  You have to give a
/// function as @FA{callback}. It will get a request and response object as its
/// arguments
////////////////////////////////////////////////////////////////////////////////

  head: function (route, callback) {
    'use strict';
    return this.handleRequest("head", route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_get
/// @brief Manage a `get` request
///
/// @FUN{FoxxApplication::get(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `get`.
///
/// When defining a route you can also define a so called 'parameterized'
/// @FA{path} like `/goose/:barn`. In this case you can later get the value
/// the user provided for `barn` via the `params` function (see the Request
/// object).
///
/// @EXAMPLES
///
/// @code
///     app.get('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  get: function (route, callback) {
    'use strict';
    return this.handleRequest("get", route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_post
/// @brief Tackle a `post` request
///
/// @FUN{FoxxApplication::post(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `post`.  See above for the
/// arguments you can give.
///
/// @EXAMPLES
///
/// @code
///     app.post('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  post: function (route, callback) {
    'use strict';
    return this.handleRequest("post", route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_put
/// @brief Sort out a `put` request
///
/// @FUN{FoxxApplication::put(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `put`.  See above for the arguments
/// you can give.
///
/// @EXAMPLES
///
/// @code
///     app.put('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  put: function (route, callback) {
    'use strict';
    return this.handleRequest("put", route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_patch
/// @brief Take charge of a `patch` request
///
/// @FUN{FoxxApplication::patch(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `patch`.  See above for the
/// arguments you can give.
///
/// @EXAMPLES
///
/// @code
///     app.patch('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  patch: function (route, callback) {
    'use strict';
    return this.handleRequest("patch", route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_delete
/// @brief Respond to a `delete` request
///
/// @FUN{FoxxApplication::delete(@FA{path}, @FA{callback})}
///
/// This handles requests from the HTTP verb `delete`.  See above for the
/// arguments you can give.
/// 
/// @warning Do not forget that `delete` is a reserved word in JavaScript and
/// therefore needs to be called as app['delete']. There is also an alias `del`
/// for this very reason.
///
/// @EXAMPLES
///
/// @code
///     app['delete']('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
///
///     app.del('/goose/barn', function (req, res) {
///       // Take this request and deal with it!
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  'delete': function (route, callback) {
    'use strict';
    return this.handleRequest("delete", route, callback);
  },

  del: function (route, callback) {
    'use strict';
    return this['delete'](route, callback);
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_before
/// @brief Before
///
/// @FUN{FoxxApplication::before(@FA{path}, @FA{callback})}
///
/// The before function takes a @FA{path} on which it should watch and a
/// function that it should execute before the routing takes place. If you do
/// omit the path, the function will be executed before each request, no matter
/// the path.  Your function gets a Request and a Response object.
///
/// @EXAMPLES
///
/// @code
///     app.before('/high/way', function(req, res) {
///       //Do some crazy request logging
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  before: function (path, func, options) {
    'use strict';
    if (is.notExisty(func)) {
      func = path;
      path = "/*";
    }

    options = options || {};

    this.routingInfo.middleware.push({
      priority: options.priority || 1,
      url: {match: path},
      action: {
        callback: function (req, res, opts, next) {
          var result = func(req, res, opts);
          if (result || !options.honorResult) {
            next();
          }
        }
      }
    });
  },

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_application_after
/// @brief After
///
/// @FUN{FoxxApplication::after(@FA{path}, @FA{callback})}
///
/// This works pretty similar to the before function.  But it acts after the
/// execution of the handlers (Big surprise, I suppose).
///
/// @EXAMPLES
///
/// @code
///     app.after('/high/way', function(req, res) {
///       //Do some crazy response logging
///     });
/// @endcode
////////////////////////////////////////////////////////////////////////////////

  after: function (path, func) {
    'use strict';
    if (is.notExisty(func)) {
      func = path;
      path = "/*";
    }

    this.routingInfo.middleware.push({
      priority: 2,
      url: {match: path},
      action: {
        callback: function (req, res, opts, next) { next(); func(req, res, opts); }
      }
    });
  }
});

exports.Application = Application;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

/// Local Variables:
/// mode: outline-minor
/// outline-regexp: "/// @brief\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\|/\\*jslint"
/// End: