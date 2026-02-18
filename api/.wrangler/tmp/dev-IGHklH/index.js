var __defProp = Object.defineProperty;
var __name = (target, value) => __defProp(target, "name", { value, configurable: true });

// node_modules/hono/dist/compose.js
var compose = /* @__PURE__ */ __name((middleware, onError, onNotFound) => {
  return (context, next) => {
    let index = -1;
    return dispatch(0);
    async function dispatch(i) {
      if (i <= index) {
        throw new Error("next() called multiple times");
      }
      index = i;
      let res;
      let isError = false;
      let handler;
      if (middleware[i]) {
        handler = middleware[i][0][0];
        context.req.routeIndex = i;
      } else {
        handler = i === middleware.length && next || void 0;
      }
      if (handler) {
        try {
          res = await handler(context, () => dispatch(i + 1));
        } catch (err) {
          if (err instanceof Error && onError) {
            context.error = err;
            res = await onError(err, context);
            isError = true;
          } else {
            throw err;
          }
        }
      } else {
        if (context.finalized === false && onNotFound) {
          res = await onNotFound(context);
        }
      }
      if (res && (context.finalized === false || isError)) {
        context.res = res;
      }
      return context;
    }
    __name(dispatch, "dispatch");
  };
}, "compose");

// node_modules/hono/dist/request/constants.js
var GET_MATCH_RESULT = /* @__PURE__ */ Symbol();

// node_modules/hono/dist/utils/body.js
var parseBody = /* @__PURE__ */ __name(async (request, options = /* @__PURE__ */ Object.create(null)) => {
  const { all = false, dot = false } = options;
  const headers = request instanceof HonoRequest ? request.raw.headers : request.headers;
  const contentType = headers.get("Content-Type");
  if (contentType?.startsWith("multipart/form-data") || contentType?.startsWith("application/x-www-form-urlencoded")) {
    return parseFormData(request, { all, dot });
  }
  return {};
}, "parseBody");
async function parseFormData(request, options) {
  const formData = await request.formData();
  if (formData) {
    return convertFormDataToBodyData(formData, options);
  }
  return {};
}
__name(parseFormData, "parseFormData");
function convertFormDataToBodyData(formData, options) {
  const form = /* @__PURE__ */ Object.create(null);
  formData.forEach((value, key) => {
    const shouldParseAllValues = options.all || key.endsWith("[]");
    if (!shouldParseAllValues) {
      form[key] = value;
    } else {
      handleParsingAllValues(form, key, value);
    }
  });
  if (options.dot) {
    Object.entries(form).forEach(([key, value]) => {
      const shouldParseDotValues = key.includes(".");
      if (shouldParseDotValues) {
        handleParsingNestedValues(form, key, value);
        delete form[key];
      }
    });
  }
  return form;
}
__name(convertFormDataToBodyData, "convertFormDataToBodyData");
var handleParsingAllValues = /* @__PURE__ */ __name((form, key, value) => {
  if (form[key] !== void 0) {
    if (Array.isArray(form[key])) {
      ;
      form[key].push(value);
    } else {
      form[key] = [form[key], value];
    }
  } else {
    if (!key.endsWith("[]")) {
      form[key] = value;
    } else {
      form[key] = [value];
    }
  }
}, "handleParsingAllValues");
var handleParsingNestedValues = /* @__PURE__ */ __name((form, key, value) => {
  let nestedForm = form;
  const keys = key.split(".");
  keys.forEach((key2, index) => {
    if (index === keys.length - 1) {
      nestedForm[key2] = value;
    } else {
      if (!nestedForm[key2] || typeof nestedForm[key2] !== "object" || Array.isArray(nestedForm[key2]) || nestedForm[key2] instanceof File) {
        nestedForm[key2] = /* @__PURE__ */ Object.create(null);
      }
      nestedForm = nestedForm[key2];
    }
  });
}, "handleParsingNestedValues");

// node_modules/hono/dist/utils/url.js
var splitPath = /* @__PURE__ */ __name((path) => {
  const paths = path.split("/");
  if (paths[0] === "") {
    paths.shift();
  }
  return paths;
}, "splitPath");
var splitRoutingPath = /* @__PURE__ */ __name((routePath) => {
  const { groups, path } = extractGroupsFromPath(routePath);
  const paths = splitPath(path);
  return replaceGroupMarks(paths, groups);
}, "splitRoutingPath");
var extractGroupsFromPath = /* @__PURE__ */ __name((path) => {
  const groups = [];
  path = path.replace(/\{[^}]+\}/g, (match2, index) => {
    const mark = `@${index}`;
    groups.push([mark, match2]);
    return mark;
  });
  return { groups, path };
}, "extractGroupsFromPath");
var replaceGroupMarks = /* @__PURE__ */ __name((paths, groups) => {
  for (let i = groups.length - 1; i >= 0; i--) {
    const [mark] = groups[i];
    for (let j = paths.length - 1; j >= 0; j--) {
      if (paths[j].includes(mark)) {
        paths[j] = paths[j].replace(mark, groups[i][1]);
        break;
      }
    }
  }
  return paths;
}, "replaceGroupMarks");
var patternCache = {};
var getPattern = /* @__PURE__ */ __name((label, next) => {
  if (label === "*") {
    return "*";
  }
  const match2 = label.match(/^\:([^\{\}]+)(?:\{(.+)\})?$/);
  if (match2) {
    const cacheKey = `${label}#${next}`;
    if (!patternCache[cacheKey]) {
      if (match2[2]) {
        patternCache[cacheKey] = next && next[0] !== ":" && next[0] !== "*" ? [cacheKey, match2[1], new RegExp(`^${match2[2]}(?=/${next})`)] : [label, match2[1], new RegExp(`^${match2[2]}$`)];
      } else {
        patternCache[cacheKey] = [label, match2[1], true];
      }
    }
    return patternCache[cacheKey];
  }
  return null;
}, "getPattern");
var tryDecode = /* @__PURE__ */ __name((str, decoder2) => {
  try {
    return decoder2(str);
  } catch {
    return str.replace(/(?:%[0-9A-Fa-f]{2})+/g, (match2) => {
      try {
        return decoder2(match2);
      } catch {
        return match2;
      }
    });
  }
}, "tryDecode");
var tryDecodeURI = /* @__PURE__ */ __name((str) => tryDecode(str, decodeURI), "tryDecodeURI");
var getPath = /* @__PURE__ */ __name((request) => {
  const url = request.url;
  const start = url.indexOf("/", url.indexOf(":") + 4);
  let i = start;
  for (; i < url.length; i++) {
    const charCode = url.charCodeAt(i);
    if (charCode === 37) {
      const queryIndex = url.indexOf("?", i);
      const hashIndex = url.indexOf("#", i);
      const end = queryIndex === -1 ? hashIndex === -1 ? void 0 : hashIndex : hashIndex === -1 ? queryIndex : Math.min(queryIndex, hashIndex);
      const path = url.slice(start, end);
      return tryDecodeURI(path.includes("%25") ? path.replace(/%25/g, "%2525") : path);
    } else if (charCode === 63 || charCode === 35) {
      break;
    }
  }
  return url.slice(start, i);
}, "getPath");
var getPathNoStrict = /* @__PURE__ */ __name((request) => {
  const result = getPath(request);
  return result.length > 1 && result.at(-1) === "/" ? result.slice(0, -1) : result;
}, "getPathNoStrict");
var mergePath = /* @__PURE__ */ __name((base, sub, ...rest) => {
  if (rest.length) {
    sub = mergePath(sub, ...rest);
  }
  return `${base?.[0] === "/" ? "" : "/"}${base}${sub === "/" ? "" : `${base?.at(-1) === "/" ? "" : "/"}${sub?.[0] === "/" ? sub.slice(1) : sub}`}`;
}, "mergePath");
var checkOptionalParameter = /* @__PURE__ */ __name((path) => {
  if (path.charCodeAt(path.length - 1) !== 63 || !path.includes(":")) {
    return null;
  }
  const segments = path.split("/");
  const results = [];
  let basePath = "";
  segments.forEach((segment) => {
    if (segment !== "" && !/\:/.test(segment)) {
      basePath += "/" + segment;
    } else if (/\:/.test(segment)) {
      if (/\?/.test(segment)) {
        if (results.length === 0 && basePath === "") {
          results.push("/");
        } else {
          results.push(basePath);
        }
        const optionalSegment = segment.replace("?", "");
        basePath += "/" + optionalSegment;
        results.push(basePath);
      } else {
        basePath += "/" + segment;
      }
    }
  });
  return results.filter((v, i, a) => a.indexOf(v) === i);
}, "checkOptionalParameter");
var _decodeURI = /* @__PURE__ */ __name((value) => {
  if (!/[%+]/.test(value)) {
    return value;
  }
  if (value.indexOf("+") !== -1) {
    value = value.replace(/\+/g, " ");
  }
  return value.indexOf("%") !== -1 ? tryDecode(value, decodeURIComponent_) : value;
}, "_decodeURI");
var _getQueryParam = /* @__PURE__ */ __name((url, key, multiple) => {
  let encoded;
  if (!multiple && key && !/[%+]/.test(key)) {
    let keyIndex2 = url.indexOf("?", 8);
    if (keyIndex2 === -1) {
      return void 0;
    }
    if (!url.startsWith(key, keyIndex2 + 1)) {
      keyIndex2 = url.indexOf(`&${key}`, keyIndex2 + 1);
    }
    while (keyIndex2 !== -1) {
      const trailingKeyCode = url.charCodeAt(keyIndex2 + key.length + 1);
      if (trailingKeyCode === 61) {
        const valueIndex = keyIndex2 + key.length + 2;
        const endIndex = url.indexOf("&", valueIndex);
        return _decodeURI(url.slice(valueIndex, endIndex === -1 ? void 0 : endIndex));
      } else if (trailingKeyCode == 38 || isNaN(trailingKeyCode)) {
        return "";
      }
      keyIndex2 = url.indexOf(`&${key}`, keyIndex2 + 1);
    }
    encoded = /[%+]/.test(url);
    if (!encoded) {
      return void 0;
    }
  }
  const results = {};
  encoded ??= /[%+]/.test(url);
  let keyIndex = url.indexOf("?", 8);
  while (keyIndex !== -1) {
    const nextKeyIndex = url.indexOf("&", keyIndex + 1);
    let valueIndex = url.indexOf("=", keyIndex);
    if (valueIndex > nextKeyIndex && nextKeyIndex !== -1) {
      valueIndex = -1;
    }
    let name = url.slice(
      keyIndex + 1,
      valueIndex === -1 ? nextKeyIndex === -1 ? void 0 : nextKeyIndex : valueIndex
    );
    if (encoded) {
      name = _decodeURI(name);
    }
    keyIndex = nextKeyIndex;
    if (name === "") {
      continue;
    }
    let value;
    if (valueIndex === -1) {
      value = "";
    } else {
      value = url.slice(valueIndex + 1, nextKeyIndex === -1 ? void 0 : nextKeyIndex);
      if (encoded) {
        value = _decodeURI(value);
      }
    }
    if (multiple) {
      if (!(results[name] && Array.isArray(results[name]))) {
        results[name] = [];
      }
      ;
      results[name].push(value);
    } else {
      results[name] ??= value;
    }
  }
  return key ? results[key] : results;
}, "_getQueryParam");
var getQueryParam = _getQueryParam;
var getQueryParams = /* @__PURE__ */ __name((url, key) => {
  return _getQueryParam(url, key, true);
}, "getQueryParams");
var decodeURIComponent_ = decodeURIComponent;

// node_modules/hono/dist/request.js
var tryDecodeURIComponent = /* @__PURE__ */ __name((str) => tryDecode(str, decodeURIComponent_), "tryDecodeURIComponent");
var HonoRequest = class {
  static {
    __name(this, "HonoRequest");
  }
  /**
   * `.raw` can get the raw Request object.
   *
   * @see {@link https://hono.dev/docs/api/request#raw}
   *
   * @example
   * ```ts
   * // For Cloudflare Workers
   * app.post('/', async (c) => {
   *   const metadata = c.req.raw.cf?.hostMetadata?
   *   ...
   * })
   * ```
   */
  raw;
  #validatedData;
  // Short name of validatedData
  #matchResult;
  routeIndex = 0;
  /**
   * `.path` can get the pathname of the request.
   *
   * @see {@link https://hono.dev/docs/api/request#path}
   *
   * @example
   * ```ts
   * app.get('/about/me', (c) => {
   *   const pathname = c.req.path // `/about/me`
   * })
   * ```
   */
  path;
  bodyCache = {};
  constructor(request, path = "/", matchResult = [[]]) {
    this.raw = request;
    this.path = path;
    this.#matchResult = matchResult;
    this.#validatedData = {};
  }
  param(key) {
    return key ? this.#getDecodedParam(key) : this.#getAllDecodedParams();
  }
  #getDecodedParam(key) {
    const paramKey = this.#matchResult[0][this.routeIndex][1][key];
    const param = this.#getParamValue(paramKey);
    return param && /\%/.test(param) ? tryDecodeURIComponent(param) : param;
  }
  #getAllDecodedParams() {
    const decoded = {};
    const keys = Object.keys(this.#matchResult[0][this.routeIndex][1]);
    for (const key of keys) {
      const value = this.#getParamValue(this.#matchResult[0][this.routeIndex][1][key]);
      if (value !== void 0) {
        decoded[key] = /\%/.test(value) ? tryDecodeURIComponent(value) : value;
      }
    }
    return decoded;
  }
  #getParamValue(paramKey) {
    return this.#matchResult[1] ? this.#matchResult[1][paramKey] : paramKey;
  }
  query(key) {
    return getQueryParam(this.url, key);
  }
  queries(key) {
    return getQueryParams(this.url, key);
  }
  header(name) {
    if (name) {
      return this.raw.headers.get(name) ?? void 0;
    }
    const headerData = {};
    this.raw.headers.forEach((value, key) => {
      headerData[key] = value;
    });
    return headerData;
  }
  async parseBody(options) {
    return this.bodyCache.parsedBody ??= await parseBody(this, options);
  }
  #cachedBody = /* @__PURE__ */ __name((key) => {
    const { bodyCache, raw: raw2 } = this;
    const cachedBody = bodyCache[key];
    if (cachedBody) {
      return cachedBody;
    }
    const anyCachedKey = Object.keys(bodyCache)[0];
    if (anyCachedKey) {
      return bodyCache[anyCachedKey].then((body) => {
        if (anyCachedKey === "json") {
          body = JSON.stringify(body);
        }
        return new Response(body)[key]();
      });
    }
    return bodyCache[key] = raw2[key]();
  }, "#cachedBody");
  /**
   * `.json()` can parse Request body of type `application/json`
   *
   * @see {@link https://hono.dev/docs/api/request#json}
   *
   * @example
   * ```ts
   * app.post('/entry', async (c) => {
   *   const body = await c.req.json()
   * })
   * ```
   */
  json() {
    return this.#cachedBody("text").then((text) => JSON.parse(text));
  }
  /**
   * `.text()` can parse Request body of type `text/plain`
   *
   * @see {@link https://hono.dev/docs/api/request#text}
   *
   * @example
   * ```ts
   * app.post('/entry', async (c) => {
   *   const body = await c.req.text()
   * })
   * ```
   */
  text() {
    return this.#cachedBody("text");
  }
  /**
   * `.arrayBuffer()` parse Request body as an `ArrayBuffer`
   *
   * @see {@link https://hono.dev/docs/api/request#arraybuffer}
   *
   * @example
   * ```ts
   * app.post('/entry', async (c) => {
   *   const body = await c.req.arrayBuffer()
   * })
   * ```
   */
  arrayBuffer() {
    return this.#cachedBody("arrayBuffer");
  }
  /**
   * Parses the request body as a `Blob`.
   * @example
   * ```ts
   * app.post('/entry', async (c) => {
   *   const body = await c.req.blob();
   * });
   * ```
   * @see https://hono.dev/docs/api/request#blob
   */
  blob() {
    return this.#cachedBody("blob");
  }
  /**
   * Parses the request body as `FormData`.
   * @example
   * ```ts
   * app.post('/entry', async (c) => {
   *   const body = await c.req.formData();
   * });
   * ```
   * @see https://hono.dev/docs/api/request#formdata
   */
  formData() {
    return this.#cachedBody("formData");
  }
  /**
   * Adds validated data to the request.
   *
   * @param target - The target of the validation.
   * @param data - The validated data to add.
   */
  addValidatedData(target, data) {
    this.#validatedData[target] = data;
  }
  valid(target) {
    return this.#validatedData[target];
  }
  /**
   * `.url()` can get the request url strings.
   *
   * @see {@link https://hono.dev/docs/api/request#url}
   *
   * @example
   * ```ts
   * app.get('/about/me', (c) => {
   *   const url = c.req.url // `http://localhost:8787/about/me`
   *   ...
   * })
   * ```
   */
  get url() {
    return this.raw.url;
  }
  /**
   * `.method()` can get the method name of the request.
   *
   * @see {@link https://hono.dev/docs/api/request#method}
   *
   * @example
   * ```ts
   * app.get('/about/me', (c) => {
   *   const method = c.req.method // `GET`
   * })
   * ```
   */
  get method() {
    return this.raw.method;
  }
  get [GET_MATCH_RESULT]() {
    return this.#matchResult;
  }
  /**
   * `.matchedRoutes()` can return a matched route in the handler
   *
   * @deprecated
   *
   * Use matchedRoutes helper defined in "hono/route" instead.
   *
   * @see {@link https://hono.dev/docs/api/request#matchedroutes}
   *
   * @example
   * ```ts
   * app.use('*', async function logger(c, next) {
   *   await next()
   *   c.req.matchedRoutes.forEach(({ handler, method, path }, i) => {
   *     const name = handler.name || (handler.length < 2 ? '[handler]' : '[middleware]')
   *     console.log(
   *       method,
   *       ' ',
   *       path,
   *       ' '.repeat(Math.max(10 - path.length, 0)),
   *       name,
   *       i === c.req.routeIndex ? '<- respond from here' : ''
   *     )
   *   })
   * })
   * ```
   */
  get matchedRoutes() {
    return this.#matchResult[0].map(([[, route]]) => route);
  }
  /**
   * `routePath()` can retrieve the path registered within the handler
   *
   * @deprecated
   *
   * Use routePath helper defined in "hono/route" instead.
   *
   * @see {@link https://hono.dev/docs/api/request#routepath}
   *
   * @example
   * ```ts
   * app.get('/posts/:id', (c) => {
   *   return c.json({ path: c.req.routePath })
   * })
   * ```
   */
  get routePath() {
    return this.#matchResult[0].map(([[, route]]) => route)[this.routeIndex].path;
  }
};

// node_modules/hono/dist/utils/html.js
var HtmlEscapedCallbackPhase = {
  Stringify: 1,
  BeforeStream: 2,
  Stream: 3
};
var raw = /* @__PURE__ */ __name((value, callbacks) => {
  const escapedString = new String(value);
  escapedString.isEscaped = true;
  escapedString.callbacks = callbacks;
  return escapedString;
}, "raw");
var resolveCallback = /* @__PURE__ */ __name(async (str, phase, preserveCallbacks, context, buffer) => {
  if (typeof str === "object" && !(str instanceof String)) {
    if (!(str instanceof Promise)) {
      str = str.toString();
    }
    if (str instanceof Promise) {
      str = await str;
    }
  }
  const callbacks = str.callbacks;
  if (!callbacks?.length) {
    return Promise.resolve(str);
  }
  if (buffer) {
    buffer[0] += str;
  } else {
    buffer = [str];
  }
  const resStr = Promise.all(callbacks.map((c) => c({ phase, buffer, context }))).then(
    (res) => Promise.all(
      res.filter(Boolean).map((str2) => resolveCallback(str2, phase, false, context, buffer))
    ).then(() => buffer[0])
  );
  if (preserveCallbacks) {
    return raw(await resStr, callbacks);
  } else {
    return resStr;
  }
}, "resolveCallback");

// node_modules/hono/dist/context.js
var TEXT_PLAIN = "text/plain; charset=UTF-8";
var setDefaultContentType = /* @__PURE__ */ __name((contentType, headers) => {
  return {
    "Content-Type": contentType,
    ...headers
  };
}, "setDefaultContentType");
var Context = class {
  static {
    __name(this, "Context");
  }
  #rawRequest;
  #req;
  /**
   * `.env` can get bindings (environment variables, secrets, KV namespaces, D1 database, R2 bucket etc.) in Cloudflare Workers.
   *
   * @see {@link https://hono.dev/docs/api/context#env}
   *
   * @example
   * ```ts
   * // Environment object for Cloudflare Workers
   * app.get('*', async c => {
   *   const counter = c.env.COUNTER
   * })
   * ```
   */
  env = {};
  #var;
  finalized = false;
  /**
   * `.error` can get the error object from the middleware if the Handler throws an error.
   *
   * @see {@link https://hono.dev/docs/api/context#error}
   *
   * @example
   * ```ts
   * app.use('*', async (c, next) => {
   *   await next()
   *   if (c.error) {
   *     // do something...
   *   }
   * })
   * ```
   */
  error;
  #status;
  #executionCtx;
  #res;
  #layout;
  #renderer;
  #notFoundHandler;
  #preparedHeaders;
  #matchResult;
  #path;
  /**
   * Creates an instance of the Context class.
   *
   * @param req - The Request object.
   * @param options - Optional configuration options for the context.
   */
  constructor(req, options) {
    this.#rawRequest = req;
    if (options) {
      this.#executionCtx = options.executionCtx;
      this.env = options.env;
      this.#notFoundHandler = options.notFoundHandler;
      this.#path = options.path;
      this.#matchResult = options.matchResult;
    }
  }
  /**
   * `.req` is the instance of {@link HonoRequest}.
   */
  get req() {
    this.#req ??= new HonoRequest(this.#rawRequest, this.#path, this.#matchResult);
    return this.#req;
  }
  /**
   * @see {@link https://hono.dev/docs/api/context#event}
   * The FetchEvent associated with the current request.
   *
   * @throws Will throw an error if the context does not have a FetchEvent.
   */
  get event() {
    if (this.#executionCtx && "respondWith" in this.#executionCtx) {
      return this.#executionCtx;
    } else {
      throw Error("This context has no FetchEvent");
    }
  }
  /**
   * @see {@link https://hono.dev/docs/api/context#executionctx}
   * The ExecutionContext associated with the current request.
   *
   * @throws Will throw an error if the context does not have an ExecutionContext.
   */
  get executionCtx() {
    if (this.#executionCtx) {
      return this.#executionCtx;
    } else {
      throw Error("This context has no ExecutionContext");
    }
  }
  /**
   * @see {@link https://hono.dev/docs/api/context#res}
   * The Response object for the current request.
   */
  get res() {
    return this.#res ||= new Response(null, {
      headers: this.#preparedHeaders ??= new Headers()
    });
  }
  /**
   * Sets the Response object for the current request.
   *
   * @param _res - The Response object to set.
   */
  set res(_res) {
    if (this.#res && _res) {
      _res = new Response(_res.body, _res);
      for (const [k, v] of this.#res.headers.entries()) {
        if (k === "content-type") {
          continue;
        }
        if (k === "set-cookie") {
          const cookies = this.#res.headers.getSetCookie();
          _res.headers.delete("set-cookie");
          for (const cookie of cookies) {
            _res.headers.append("set-cookie", cookie);
          }
        } else {
          _res.headers.set(k, v);
        }
      }
    }
    this.#res = _res;
    this.finalized = true;
  }
  /**
   * `.render()` can create a response within a layout.
   *
   * @see {@link https://hono.dev/docs/api/context#render-setrenderer}
   *
   * @example
   * ```ts
   * app.get('/', (c) => {
   *   return c.render('Hello!')
   * })
   * ```
   */
  render = /* @__PURE__ */ __name((...args) => {
    this.#renderer ??= (content) => this.html(content);
    return this.#renderer(...args);
  }, "render");
  /**
   * Sets the layout for the response.
   *
   * @param layout - The layout to set.
   * @returns The layout function.
   */
  setLayout = /* @__PURE__ */ __name((layout) => this.#layout = layout, "setLayout");
  /**
   * Gets the current layout for the response.
   *
   * @returns The current layout function.
   */
  getLayout = /* @__PURE__ */ __name(() => this.#layout, "getLayout");
  /**
   * `.setRenderer()` can set the layout in the custom middleware.
   *
   * @see {@link https://hono.dev/docs/api/context#render-setrenderer}
   *
   * @example
   * ```tsx
   * app.use('*', async (c, next) => {
   *   c.setRenderer((content) => {
   *     return c.html(
   *       <html>
   *         <body>
   *           <p>{content}</p>
   *         </body>
   *       </html>
   *     )
   *   })
   *   await next()
   * })
   * ```
   */
  setRenderer = /* @__PURE__ */ __name((renderer) => {
    this.#renderer = renderer;
  }, "setRenderer");
  /**
   * `.header()` can set headers.
   *
   * @see {@link https://hono.dev/docs/api/context#header}
   *
   * @example
   * ```ts
   * app.get('/welcome', (c) => {
   *   // Set headers
   *   c.header('X-Message', 'Hello!')
   *   c.header('Content-Type', 'text/plain')
   *
   *   return c.body('Thank you for coming')
   * })
   * ```
   */
  header = /* @__PURE__ */ __name((name, value, options) => {
    if (this.finalized) {
      this.#res = new Response(this.#res.body, this.#res);
    }
    const headers = this.#res ? this.#res.headers : this.#preparedHeaders ??= new Headers();
    if (value === void 0) {
      headers.delete(name);
    } else if (options?.append) {
      headers.append(name, value);
    } else {
      headers.set(name, value);
    }
  }, "header");
  status = /* @__PURE__ */ __name((status) => {
    this.#status = status;
  }, "status");
  /**
   * `.set()` can set the value specified by the key.
   *
   * @see {@link https://hono.dev/docs/api/context#set-get}
   *
   * @example
   * ```ts
   * app.use('*', async (c, next) => {
   *   c.set('message', 'Hono is hot!!')
   *   await next()
   * })
   * ```
   */
  set = /* @__PURE__ */ __name((key, value) => {
    this.#var ??= /* @__PURE__ */ new Map();
    this.#var.set(key, value);
  }, "set");
  /**
   * `.get()` can use the value specified by the key.
   *
   * @see {@link https://hono.dev/docs/api/context#set-get}
   *
   * @example
   * ```ts
   * app.get('/', (c) => {
   *   const message = c.get('message')
   *   return c.text(`The message is "${message}"`)
   * })
   * ```
   */
  get = /* @__PURE__ */ __name((key) => {
    return this.#var ? this.#var.get(key) : void 0;
  }, "get");
  /**
   * `.var` can access the value of a variable.
   *
   * @see {@link https://hono.dev/docs/api/context#var}
   *
   * @example
   * ```ts
   * const result = c.var.client.oneMethod()
   * ```
   */
  // c.var.propName is a read-only
  get var() {
    if (!this.#var) {
      return {};
    }
    return Object.fromEntries(this.#var);
  }
  #newResponse(data, arg, headers) {
    const responseHeaders = this.#res ? new Headers(this.#res.headers) : this.#preparedHeaders ?? new Headers();
    if (typeof arg === "object" && "headers" in arg) {
      const argHeaders = arg.headers instanceof Headers ? arg.headers : new Headers(arg.headers);
      for (const [key, value] of argHeaders) {
        if (key.toLowerCase() === "set-cookie") {
          responseHeaders.append(key, value);
        } else {
          responseHeaders.set(key, value);
        }
      }
    }
    if (headers) {
      for (const [k, v] of Object.entries(headers)) {
        if (typeof v === "string") {
          responseHeaders.set(k, v);
        } else {
          responseHeaders.delete(k);
          for (const v2 of v) {
            responseHeaders.append(k, v2);
          }
        }
      }
    }
    const status = typeof arg === "number" ? arg : arg?.status ?? this.#status;
    return new Response(data, { status, headers: responseHeaders });
  }
  newResponse = /* @__PURE__ */ __name((...args) => this.#newResponse(...args), "newResponse");
  /**
   * `.body()` can return the HTTP response.
   * You can set headers with `.header()` and set HTTP status code with `.status`.
   * This can also be set in `.text()`, `.json()` and so on.
   *
   * @see {@link https://hono.dev/docs/api/context#body}
   *
   * @example
   * ```ts
   * app.get('/welcome', (c) => {
   *   // Set headers
   *   c.header('X-Message', 'Hello!')
   *   c.header('Content-Type', 'text/plain')
   *   // Set HTTP status code
   *   c.status(201)
   *
   *   // Return the response body
   *   return c.body('Thank you for coming')
   * })
   * ```
   */
  body = /* @__PURE__ */ __name((data, arg, headers) => this.#newResponse(data, arg, headers), "body");
  /**
   * `.text()` can render text as `Content-Type:text/plain`.
   *
   * @see {@link https://hono.dev/docs/api/context#text}
   *
   * @example
   * ```ts
   * app.get('/say', (c) => {
   *   return c.text('Hello!')
   * })
   * ```
   */
  text = /* @__PURE__ */ __name((text, arg, headers) => {
    return !this.#preparedHeaders && !this.#status && !arg && !headers && !this.finalized ? new Response(text) : this.#newResponse(
      text,
      arg,
      setDefaultContentType(TEXT_PLAIN, headers)
    );
  }, "text");
  /**
   * `.json()` can render JSON as `Content-Type:application/json`.
   *
   * @see {@link https://hono.dev/docs/api/context#json}
   *
   * @example
   * ```ts
   * app.get('/api', (c) => {
   *   return c.json({ message: 'Hello!' })
   * })
   * ```
   */
  json = /* @__PURE__ */ __name((object, arg, headers) => {
    return this.#newResponse(
      JSON.stringify(object),
      arg,
      setDefaultContentType("application/json", headers)
    );
  }, "json");
  html = /* @__PURE__ */ __name((html, arg, headers) => {
    const res = /* @__PURE__ */ __name((html2) => this.#newResponse(html2, arg, setDefaultContentType("text/html; charset=UTF-8", headers)), "res");
    return typeof html === "object" ? resolveCallback(html, HtmlEscapedCallbackPhase.Stringify, false, {}).then(res) : res(html);
  }, "html");
  /**
   * `.redirect()` can Redirect, default status code is 302.
   *
   * @see {@link https://hono.dev/docs/api/context#redirect}
   *
   * @example
   * ```ts
   * app.get('/redirect', (c) => {
   *   return c.redirect('/')
   * })
   * app.get('/redirect-permanently', (c) => {
   *   return c.redirect('/', 301)
   * })
   * ```
   */
  redirect = /* @__PURE__ */ __name((location, status) => {
    const locationString = String(location);
    this.header(
      "Location",
      // Multibyes should be encoded
      // eslint-disable-next-line no-control-regex
      !/[^\x00-\xFF]/.test(locationString) ? locationString : encodeURI(locationString)
    );
    return this.newResponse(null, status ?? 302);
  }, "redirect");
  /**
   * `.notFound()` can return the Not Found Response.
   *
   * @see {@link https://hono.dev/docs/api/context#notfound}
   *
   * @example
   * ```ts
   * app.get('/notfound', (c) => {
   *   return c.notFound()
   * })
   * ```
   */
  notFound = /* @__PURE__ */ __name(() => {
    this.#notFoundHandler ??= () => new Response();
    return this.#notFoundHandler(this);
  }, "notFound");
};

// node_modules/hono/dist/router.js
var METHOD_NAME_ALL = "ALL";
var METHOD_NAME_ALL_LOWERCASE = "all";
var METHODS = ["get", "post", "put", "delete", "options", "patch"];
var MESSAGE_MATCHER_IS_ALREADY_BUILT = "Can not add a route since the matcher is already built.";
var UnsupportedPathError = class extends Error {
  static {
    __name(this, "UnsupportedPathError");
  }
};

// node_modules/hono/dist/utils/constants.js
var COMPOSED_HANDLER = "__COMPOSED_HANDLER";

// node_modules/hono/dist/hono-base.js
var notFoundHandler = /* @__PURE__ */ __name((c) => {
  return c.text("404 Not Found", 404);
}, "notFoundHandler");
var errorHandler = /* @__PURE__ */ __name((err, c) => {
  if ("getResponse" in err) {
    const res = err.getResponse();
    return c.newResponse(res.body, res);
  }
  console.error(err);
  return c.text("Internal Server Error", 500);
}, "errorHandler");
var Hono = class _Hono {
  static {
    __name(this, "_Hono");
  }
  get;
  post;
  put;
  delete;
  options;
  patch;
  all;
  on;
  use;
  /*
    This class is like an abstract class and does not have a router.
    To use it, inherit the class and implement router in the constructor.
  */
  router;
  getPath;
  // Cannot use `#` because it requires visibility at JavaScript runtime.
  _basePath = "/";
  #path = "/";
  routes = [];
  constructor(options = {}) {
    const allMethods = [...METHODS, METHOD_NAME_ALL_LOWERCASE];
    allMethods.forEach((method) => {
      this[method] = (args1, ...args) => {
        if (typeof args1 === "string") {
          this.#path = args1;
        } else {
          this.#addRoute(method, this.#path, args1);
        }
        args.forEach((handler) => {
          this.#addRoute(method, this.#path, handler);
        });
        return this;
      };
    });
    this.on = (method, path, ...handlers) => {
      for (const p of [path].flat()) {
        this.#path = p;
        for (const m of [method].flat()) {
          handlers.map((handler) => {
            this.#addRoute(m.toUpperCase(), this.#path, handler);
          });
        }
      }
      return this;
    };
    this.use = (arg1, ...handlers) => {
      if (typeof arg1 === "string") {
        this.#path = arg1;
      } else {
        this.#path = "*";
        handlers.unshift(arg1);
      }
      handlers.forEach((handler) => {
        this.#addRoute(METHOD_NAME_ALL, this.#path, handler);
      });
      return this;
    };
    const { strict, ...optionsWithoutStrict } = options;
    Object.assign(this, optionsWithoutStrict);
    this.getPath = strict ?? true ? options.getPath ?? getPath : getPathNoStrict;
  }
  #clone() {
    const clone = new _Hono({
      router: this.router,
      getPath: this.getPath
    });
    clone.errorHandler = this.errorHandler;
    clone.#notFoundHandler = this.#notFoundHandler;
    clone.routes = this.routes;
    return clone;
  }
  #notFoundHandler = notFoundHandler;
  // Cannot use `#` because it requires visibility at JavaScript runtime.
  errorHandler = errorHandler;
  /**
   * `.route()` allows grouping other Hono instance in routes.
   *
   * @see {@link https://hono.dev/docs/api/routing#grouping}
   *
   * @param {string} path - base Path
   * @param {Hono} app - other Hono instance
   * @returns {Hono} routed Hono instance
   *
   * @example
   * ```ts
   * const app = new Hono()
   * const app2 = new Hono()
   *
   * app2.get("/user", (c) => c.text("user"))
   * app.route("/api", app2) // GET /api/user
   * ```
   */
  route(path, app2) {
    const subApp = this.basePath(path);
    app2.routes.map((r) => {
      let handler;
      if (app2.errorHandler === errorHandler) {
        handler = r.handler;
      } else {
        handler = /* @__PURE__ */ __name(async (c, next) => (await compose([], app2.errorHandler)(c, () => r.handler(c, next))).res, "handler");
        handler[COMPOSED_HANDLER] = r.handler;
      }
      subApp.#addRoute(r.method, r.path, handler);
    });
    return this;
  }
  /**
   * `.basePath()` allows base paths to be specified.
   *
   * @see {@link https://hono.dev/docs/api/routing#base-path}
   *
   * @param {string} path - base Path
   * @returns {Hono} changed Hono instance
   *
   * @example
   * ```ts
   * const api = new Hono().basePath('/api')
   * ```
   */
  basePath(path) {
    const subApp = this.#clone();
    subApp._basePath = mergePath(this._basePath, path);
    return subApp;
  }
  /**
   * `.onError()` handles an error and returns a customized Response.
   *
   * @see {@link https://hono.dev/docs/api/hono#error-handling}
   *
   * @param {ErrorHandler} handler - request Handler for error
   * @returns {Hono} changed Hono instance
   *
   * @example
   * ```ts
   * app.onError((err, c) => {
   *   console.error(`${err}`)
   *   return c.text('Custom Error Message', 500)
   * })
   * ```
   */
  onError = /* @__PURE__ */ __name((handler) => {
    this.errorHandler = handler;
    return this;
  }, "onError");
  /**
   * `.notFound()` allows you to customize a Not Found Response.
   *
   * @see {@link https://hono.dev/docs/api/hono#not-found}
   *
   * @param {NotFoundHandler} handler - request handler for not-found
   * @returns {Hono} changed Hono instance
   *
   * @example
   * ```ts
   * app.notFound((c) => {
   *   return c.text('Custom 404 Message', 404)
   * })
   * ```
   */
  notFound = /* @__PURE__ */ __name((handler) => {
    this.#notFoundHandler = handler;
    return this;
  }, "notFound");
  /**
   * `.mount()` allows you to mount applications built with other frameworks into your Hono application.
   *
   * @see {@link https://hono.dev/docs/api/hono#mount}
   *
   * @param {string} path - base Path
   * @param {Function} applicationHandler - other Request Handler
   * @param {MountOptions} [options] - options of `.mount()`
   * @returns {Hono} mounted Hono instance
   *
   * @example
   * ```ts
   * import { Router as IttyRouter } from 'itty-router'
   * import { Hono } from 'hono'
   * // Create itty-router application
   * const ittyRouter = IttyRouter()
   * // GET /itty-router/hello
   * ittyRouter.get('/hello', () => new Response('Hello from itty-router'))
   *
   * const app = new Hono()
   * app.mount('/itty-router', ittyRouter.handle)
   * ```
   *
   * @example
   * ```ts
   * const app = new Hono()
   * // Send the request to another application without modification.
   * app.mount('/app', anotherApp, {
   *   replaceRequest: (req) => req,
   * })
   * ```
   */
  mount(path, applicationHandler, options) {
    let replaceRequest;
    let optionHandler;
    if (options) {
      if (typeof options === "function") {
        optionHandler = options;
      } else {
        optionHandler = options.optionHandler;
        if (options.replaceRequest === false) {
          replaceRequest = /* @__PURE__ */ __name((request) => request, "replaceRequest");
        } else {
          replaceRequest = options.replaceRequest;
        }
      }
    }
    const getOptions = optionHandler ? (c) => {
      const options2 = optionHandler(c);
      return Array.isArray(options2) ? options2 : [options2];
    } : (c) => {
      let executionContext = void 0;
      try {
        executionContext = c.executionCtx;
      } catch {
      }
      return [c.env, executionContext];
    };
    replaceRequest ||= (() => {
      const mergedPath = mergePath(this._basePath, path);
      const pathPrefixLength = mergedPath === "/" ? 0 : mergedPath.length;
      return (request) => {
        const url = new URL(request.url);
        url.pathname = url.pathname.slice(pathPrefixLength) || "/";
        return new Request(url, request);
      };
    })();
    const handler = /* @__PURE__ */ __name(async (c, next) => {
      const res = await applicationHandler(replaceRequest(c.req.raw), ...getOptions(c));
      if (res) {
        return res;
      }
      await next();
    }, "handler");
    this.#addRoute(METHOD_NAME_ALL, mergePath(path, "*"), handler);
    return this;
  }
  #addRoute(method, path, handler) {
    method = method.toUpperCase();
    path = mergePath(this._basePath, path);
    const r = { basePath: this._basePath, path, method, handler };
    this.router.add(method, path, [handler, r]);
    this.routes.push(r);
  }
  #handleError(err, c) {
    if (err instanceof Error) {
      return this.errorHandler(err, c);
    }
    throw err;
  }
  #dispatch(request, executionCtx, env, method) {
    if (method === "HEAD") {
      return (async () => new Response(null, await this.#dispatch(request, executionCtx, env, "GET")))();
    }
    const path = this.getPath(request, { env });
    const matchResult = this.router.match(method, path);
    const c = new Context(request, {
      path,
      matchResult,
      env,
      executionCtx,
      notFoundHandler: this.#notFoundHandler
    });
    if (matchResult[0].length === 1) {
      let res;
      try {
        res = matchResult[0][0][0][0](c, async () => {
          c.res = await this.#notFoundHandler(c);
        });
      } catch (err) {
        return this.#handleError(err, c);
      }
      return res instanceof Promise ? res.then(
        (resolved) => resolved || (c.finalized ? c.res : this.#notFoundHandler(c))
      ).catch((err) => this.#handleError(err, c)) : res ?? this.#notFoundHandler(c);
    }
    const composed = compose(matchResult[0], this.errorHandler, this.#notFoundHandler);
    return (async () => {
      try {
        const context = await composed(c);
        if (!context.finalized) {
          throw new Error(
            "Context is not finalized. Did you forget to return a Response object or `await next()`?"
          );
        }
        return context.res;
      } catch (err) {
        return this.#handleError(err, c);
      }
    })();
  }
  /**
   * `.fetch()` will be entry point of your app.
   *
   * @see {@link https://hono.dev/docs/api/hono#fetch}
   *
   * @param {Request} request - request Object of request
   * @param {Env} Env - env Object
   * @param {ExecutionContext} - context of execution
   * @returns {Response | Promise<Response>} response of request
   *
   */
  fetch = /* @__PURE__ */ __name((request, ...rest) => {
    return this.#dispatch(request, rest[1], rest[0], request.method);
  }, "fetch");
  /**
   * `.request()` is a useful method for testing.
   * You can pass a URL or pathname to send a GET request.
   * app will return a Response object.
   * ```ts
   * test('GET /hello is ok', async () => {
   *   const res = await app.request('/hello')
   *   expect(res.status).toBe(200)
   * })
   * ```
   * @see https://hono.dev/docs/api/hono#request
   */
  request = /* @__PURE__ */ __name((input, requestInit, Env, executionCtx) => {
    if (input instanceof Request) {
      return this.fetch(requestInit ? new Request(input, requestInit) : input, Env, executionCtx);
    }
    input = input.toString();
    return this.fetch(
      new Request(
        /^https?:\/\//.test(input) ? input : `http://localhost${mergePath("/", input)}`,
        requestInit
      ),
      Env,
      executionCtx
    );
  }, "request");
  /**
   * `.fire()` automatically adds a global fetch event listener.
   * This can be useful for environments that adhere to the Service Worker API, such as non-ES module Cloudflare Workers.
   * @deprecated
   * Use `fire` from `hono/service-worker` instead.
   * ```ts
   * import { Hono } from 'hono'
   * import { fire } from 'hono/service-worker'
   *
   * const app = new Hono()
   * // ...
   * fire(app)
   * ```
   * @see https://hono.dev/docs/api/hono#fire
   * @see https://developer.mozilla.org/en-US/docs/Web/API/Service_Worker_API
   * @see https://developers.cloudflare.com/workers/reference/migrate-to-module-workers/
   */
  fire = /* @__PURE__ */ __name(() => {
    addEventListener("fetch", (event) => {
      event.respondWith(this.#dispatch(event.request, event, void 0, event.request.method));
    });
  }, "fire");
};

// node_modules/hono/dist/router/reg-exp-router/matcher.js
var emptyParam = [];
function match(method, path) {
  const matchers = this.buildAllMatchers();
  const match2 = /* @__PURE__ */ __name(((method2, path2) => {
    const matcher = matchers[method2] || matchers[METHOD_NAME_ALL];
    const staticMatch = matcher[2][path2];
    if (staticMatch) {
      return staticMatch;
    }
    const match3 = path2.match(matcher[0]);
    if (!match3) {
      return [[], emptyParam];
    }
    const index = match3.indexOf("", 1);
    return [matcher[1][index], match3];
  }), "match2");
  this.match = match2;
  return match2(method, path);
}
__name(match, "match");

// node_modules/hono/dist/router/reg-exp-router/node.js
var LABEL_REG_EXP_STR = "[^/]+";
var ONLY_WILDCARD_REG_EXP_STR = ".*";
var TAIL_WILDCARD_REG_EXP_STR = "(?:|/.*)";
var PATH_ERROR = /* @__PURE__ */ Symbol();
var regExpMetaChars = new Set(".\\+*[^]$()");
function compareKey(a, b) {
  if (a.length === 1) {
    return b.length === 1 ? a < b ? -1 : 1 : -1;
  }
  if (b.length === 1) {
    return 1;
  }
  if (a === ONLY_WILDCARD_REG_EXP_STR || a === TAIL_WILDCARD_REG_EXP_STR) {
    return 1;
  } else if (b === ONLY_WILDCARD_REG_EXP_STR || b === TAIL_WILDCARD_REG_EXP_STR) {
    return -1;
  }
  if (a === LABEL_REG_EXP_STR) {
    return 1;
  } else if (b === LABEL_REG_EXP_STR) {
    return -1;
  }
  return a.length === b.length ? a < b ? -1 : 1 : b.length - a.length;
}
__name(compareKey, "compareKey");
var Node = class _Node {
  static {
    __name(this, "_Node");
  }
  #index;
  #varIndex;
  #children = /* @__PURE__ */ Object.create(null);
  insert(tokens, index, paramMap, context, pathErrorCheckOnly) {
    if (tokens.length === 0) {
      if (this.#index !== void 0) {
        throw PATH_ERROR;
      }
      if (pathErrorCheckOnly) {
        return;
      }
      this.#index = index;
      return;
    }
    const [token, ...restTokens] = tokens;
    const pattern = token === "*" ? restTokens.length === 0 ? ["", "", ONLY_WILDCARD_REG_EXP_STR] : ["", "", LABEL_REG_EXP_STR] : token === "/*" ? ["", "", TAIL_WILDCARD_REG_EXP_STR] : token.match(/^\:([^\{\}]+)(?:\{(.+)\})?$/);
    let node;
    if (pattern) {
      const name = pattern[1];
      let regexpStr = pattern[2] || LABEL_REG_EXP_STR;
      if (name && pattern[2]) {
        if (regexpStr === ".*") {
          throw PATH_ERROR;
        }
        regexpStr = regexpStr.replace(/^\((?!\?:)(?=[^)]+\)$)/, "(?:");
        if (/\((?!\?:)/.test(regexpStr)) {
          throw PATH_ERROR;
        }
      }
      node = this.#children[regexpStr];
      if (!node) {
        if (Object.keys(this.#children).some(
          (k) => k !== ONLY_WILDCARD_REG_EXP_STR && k !== TAIL_WILDCARD_REG_EXP_STR
        )) {
          throw PATH_ERROR;
        }
        if (pathErrorCheckOnly) {
          return;
        }
        node = this.#children[regexpStr] = new _Node();
        if (name !== "") {
          node.#varIndex = context.varIndex++;
        }
      }
      if (!pathErrorCheckOnly && name !== "") {
        paramMap.push([name, node.#varIndex]);
      }
    } else {
      node = this.#children[token];
      if (!node) {
        if (Object.keys(this.#children).some(
          (k) => k.length > 1 && k !== ONLY_WILDCARD_REG_EXP_STR && k !== TAIL_WILDCARD_REG_EXP_STR
        )) {
          throw PATH_ERROR;
        }
        if (pathErrorCheckOnly) {
          return;
        }
        node = this.#children[token] = new _Node();
      }
    }
    node.insert(restTokens, index, paramMap, context, pathErrorCheckOnly);
  }
  buildRegExpStr() {
    const childKeys = Object.keys(this.#children).sort(compareKey);
    const strList = childKeys.map((k) => {
      const c = this.#children[k];
      return (typeof c.#varIndex === "number" ? `(${k})@${c.#varIndex}` : regExpMetaChars.has(k) ? `\\${k}` : k) + c.buildRegExpStr();
    });
    if (typeof this.#index === "number") {
      strList.unshift(`#${this.#index}`);
    }
    if (strList.length === 0) {
      return "";
    }
    if (strList.length === 1) {
      return strList[0];
    }
    return "(?:" + strList.join("|") + ")";
  }
};

// node_modules/hono/dist/router/reg-exp-router/trie.js
var Trie = class {
  static {
    __name(this, "Trie");
  }
  #context = { varIndex: 0 };
  #root = new Node();
  insert(path, index, pathErrorCheckOnly) {
    const paramAssoc = [];
    const groups = [];
    for (let i = 0; ; ) {
      let replaced = false;
      path = path.replace(/\{[^}]+\}/g, (m) => {
        const mark = `@\\${i}`;
        groups[i] = [mark, m];
        i++;
        replaced = true;
        return mark;
      });
      if (!replaced) {
        break;
      }
    }
    const tokens = path.match(/(?::[^\/]+)|(?:\/\*$)|./g) || [];
    for (let i = groups.length - 1; i >= 0; i--) {
      const [mark] = groups[i];
      for (let j = tokens.length - 1; j >= 0; j--) {
        if (tokens[j].indexOf(mark) !== -1) {
          tokens[j] = tokens[j].replace(mark, groups[i][1]);
          break;
        }
      }
    }
    this.#root.insert(tokens, index, paramAssoc, this.#context, pathErrorCheckOnly);
    return paramAssoc;
  }
  buildRegExp() {
    let regexp = this.#root.buildRegExpStr();
    if (regexp === "") {
      return [/^$/, [], []];
    }
    let captureIndex = 0;
    const indexReplacementMap = [];
    const paramReplacementMap = [];
    regexp = regexp.replace(/#(\d+)|@(\d+)|\.\*\$/g, (_, handlerIndex, paramIndex) => {
      if (handlerIndex !== void 0) {
        indexReplacementMap[++captureIndex] = Number(handlerIndex);
        return "$()";
      }
      if (paramIndex !== void 0) {
        paramReplacementMap[Number(paramIndex)] = ++captureIndex;
        return "";
      }
      return "";
    });
    return [new RegExp(`^${regexp}`), indexReplacementMap, paramReplacementMap];
  }
};

// node_modules/hono/dist/router/reg-exp-router/router.js
var nullMatcher = [/^$/, [], /* @__PURE__ */ Object.create(null)];
var wildcardRegExpCache = /* @__PURE__ */ Object.create(null);
function buildWildcardRegExp(path) {
  return wildcardRegExpCache[path] ??= new RegExp(
    path === "*" ? "" : `^${path.replace(
      /\/\*$|([.\\+*[^\]$()])/g,
      (_, metaChar) => metaChar ? `\\${metaChar}` : "(?:|/.*)"
    )}$`
  );
}
__name(buildWildcardRegExp, "buildWildcardRegExp");
function clearWildcardRegExpCache() {
  wildcardRegExpCache = /* @__PURE__ */ Object.create(null);
}
__name(clearWildcardRegExpCache, "clearWildcardRegExpCache");
function buildMatcherFromPreprocessedRoutes(routes) {
  const trie = new Trie();
  const handlerData = [];
  if (routes.length === 0) {
    return nullMatcher;
  }
  const routesWithStaticPathFlag = routes.map(
    (route) => [!/\*|\/:/.test(route[0]), ...route]
  ).sort(
    ([isStaticA, pathA], [isStaticB, pathB]) => isStaticA ? 1 : isStaticB ? -1 : pathA.length - pathB.length
  );
  const staticMap = /* @__PURE__ */ Object.create(null);
  for (let i = 0, j = -1, len = routesWithStaticPathFlag.length; i < len; i++) {
    const [pathErrorCheckOnly, path, handlers] = routesWithStaticPathFlag[i];
    if (pathErrorCheckOnly) {
      staticMap[path] = [handlers.map(([h]) => [h, /* @__PURE__ */ Object.create(null)]), emptyParam];
    } else {
      j++;
    }
    let paramAssoc;
    try {
      paramAssoc = trie.insert(path, j, pathErrorCheckOnly);
    } catch (e) {
      throw e === PATH_ERROR ? new UnsupportedPathError(path) : e;
    }
    if (pathErrorCheckOnly) {
      continue;
    }
    handlerData[j] = handlers.map(([h, paramCount]) => {
      const paramIndexMap = /* @__PURE__ */ Object.create(null);
      paramCount -= 1;
      for (; paramCount >= 0; paramCount--) {
        const [key, value] = paramAssoc[paramCount];
        paramIndexMap[key] = value;
      }
      return [h, paramIndexMap];
    });
  }
  const [regexp, indexReplacementMap, paramReplacementMap] = trie.buildRegExp();
  for (let i = 0, len = handlerData.length; i < len; i++) {
    for (let j = 0, len2 = handlerData[i].length; j < len2; j++) {
      const map = handlerData[i][j]?.[1];
      if (!map) {
        continue;
      }
      const keys = Object.keys(map);
      for (let k = 0, len3 = keys.length; k < len3; k++) {
        map[keys[k]] = paramReplacementMap[map[keys[k]]];
      }
    }
  }
  const handlerMap = [];
  for (const i in indexReplacementMap) {
    handlerMap[i] = handlerData[indexReplacementMap[i]];
  }
  return [regexp, handlerMap, staticMap];
}
__name(buildMatcherFromPreprocessedRoutes, "buildMatcherFromPreprocessedRoutes");
function findMiddleware(middleware, path) {
  if (!middleware) {
    return void 0;
  }
  for (const k of Object.keys(middleware).sort((a, b) => b.length - a.length)) {
    if (buildWildcardRegExp(k).test(path)) {
      return [...middleware[k]];
    }
  }
  return void 0;
}
__name(findMiddleware, "findMiddleware");
var RegExpRouter = class {
  static {
    __name(this, "RegExpRouter");
  }
  name = "RegExpRouter";
  #middleware;
  #routes;
  constructor() {
    this.#middleware = { [METHOD_NAME_ALL]: /* @__PURE__ */ Object.create(null) };
    this.#routes = { [METHOD_NAME_ALL]: /* @__PURE__ */ Object.create(null) };
  }
  add(method, path, handler) {
    const middleware = this.#middleware;
    const routes = this.#routes;
    if (!middleware || !routes) {
      throw new Error(MESSAGE_MATCHER_IS_ALREADY_BUILT);
    }
    if (!middleware[method]) {
      ;
      [middleware, routes].forEach((handlerMap) => {
        handlerMap[method] = /* @__PURE__ */ Object.create(null);
        Object.keys(handlerMap[METHOD_NAME_ALL]).forEach((p) => {
          handlerMap[method][p] = [...handlerMap[METHOD_NAME_ALL][p]];
        });
      });
    }
    if (path === "/*") {
      path = "*";
    }
    const paramCount = (path.match(/\/:/g) || []).length;
    if (/\*$/.test(path)) {
      const re = buildWildcardRegExp(path);
      if (method === METHOD_NAME_ALL) {
        Object.keys(middleware).forEach((m) => {
          middleware[m][path] ||= findMiddleware(middleware[m], path) || findMiddleware(middleware[METHOD_NAME_ALL], path) || [];
        });
      } else {
        middleware[method][path] ||= findMiddleware(middleware[method], path) || findMiddleware(middleware[METHOD_NAME_ALL], path) || [];
      }
      Object.keys(middleware).forEach((m) => {
        if (method === METHOD_NAME_ALL || method === m) {
          Object.keys(middleware[m]).forEach((p) => {
            re.test(p) && middleware[m][p].push([handler, paramCount]);
          });
        }
      });
      Object.keys(routes).forEach((m) => {
        if (method === METHOD_NAME_ALL || method === m) {
          Object.keys(routes[m]).forEach(
            (p) => re.test(p) && routes[m][p].push([handler, paramCount])
          );
        }
      });
      return;
    }
    const paths = checkOptionalParameter(path) || [path];
    for (let i = 0, len = paths.length; i < len; i++) {
      const path2 = paths[i];
      Object.keys(routes).forEach((m) => {
        if (method === METHOD_NAME_ALL || method === m) {
          routes[m][path2] ||= [
            ...findMiddleware(middleware[m], path2) || findMiddleware(middleware[METHOD_NAME_ALL], path2) || []
          ];
          routes[m][path2].push([handler, paramCount - len + i + 1]);
        }
      });
    }
  }
  match = match;
  buildAllMatchers() {
    const matchers = /* @__PURE__ */ Object.create(null);
    Object.keys(this.#routes).concat(Object.keys(this.#middleware)).forEach((method) => {
      matchers[method] ||= this.#buildMatcher(method);
    });
    this.#middleware = this.#routes = void 0;
    clearWildcardRegExpCache();
    return matchers;
  }
  #buildMatcher(method) {
    const routes = [];
    let hasOwnRoute = method === METHOD_NAME_ALL;
    [this.#middleware, this.#routes].forEach((r) => {
      const ownRoute = r[method] ? Object.keys(r[method]).map((path) => [path, r[method][path]]) : [];
      if (ownRoute.length !== 0) {
        hasOwnRoute ||= true;
        routes.push(...ownRoute);
      } else if (method !== METHOD_NAME_ALL) {
        routes.push(
          ...Object.keys(r[METHOD_NAME_ALL]).map((path) => [path, r[METHOD_NAME_ALL][path]])
        );
      }
    });
    if (!hasOwnRoute) {
      return null;
    } else {
      return buildMatcherFromPreprocessedRoutes(routes);
    }
  }
};

// node_modules/hono/dist/router/smart-router/router.js
var SmartRouter = class {
  static {
    __name(this, "SmartRouter");
  }
  name = "SmartRouter";
  #routers = [];
  #routes = [];
  constructor(init) {
    this.#routers = init.routers;
  }
  add(method, path, handler) {
    if (!this.#routes) {
      throw new Error(MESSAGE_MATCHER_IS_ALREADY_BUILT);
    }
    this.#routes.push([method, path, handler]);
  }
  match(method, path) {
    if (!this.#routes) {
      throw new Error("Fatal error");
    }
    const routers = this.#routers;
    const routes = this.#routes;
    const len = routers.length;
    let i = 0;
    let res;
    for (; i < len; i++) {
      const router = routers[i];
      try {
        for (let i2 = 0, len2 = routes.length; i2 < len2; i2++) {
          router.add(...routes[i2]);
        }
        res = router.match(method, path);
      } catch (e) {
        if (e instanceof UnsupportedPathError) {
          continue;
        }
        throw e;
      }
      this.match = router.match.bind(router);
      this.#routers = [router];
      this.#routes = void 0;
      break;
    }
    if (i === len) {
      throw new Error("Fatal error");
    }
    this.name = `SmartRouter + ${this.activeRouter.name}`;
    return res;
  }
  get activeRouter() {
    if (this.#routes || this.#routers.length !== 1) {
      throw new Error("No active router has been determined yet.");
    }
    return this.#routers[0];
  }
};

// node_modules/hono/dist/router/trie-router/node.js
var emptyParams = /* @__PURE__ */ Object.create(null);
var Node2 = class _Node2 {
  static {
    __name(this, "_Node");
  }
  #methods;
  #children;
  #patterns;
  #order = 0;
  #params = emptyParams;
  constructor(method, handler, children) {
    this.#children = children || /* @__PURE__ */ Object.create(null);
    this.#methods = [];
    if (method && handler) {
      const m = /* @__PURE__ */ Object.create(null);
      m[method] = { handler, possibleKeys: [], score: 0 };
      this.#methods = [m];
    }
    this.#patterns = [];
  }
  insert(method, path, handler) {
    this.#order = ++this.#order;
    let curNode = this;
    const parts = splitRoutingPath(path);
    const possibleKeys = [];
    for (let i = 0, len = parts.length; i < len; i++) {
      const p = parts[i];
      const nextP = parts[i + 1];
      const pattern = getPattern(p, nextP);
      const key = Array.isArray(pattern) ? pattern[0] : p;
      if (key in curNode.#children) {
        curNode = curNode.#children[key];
        if (pattern) {
          possibleKeys.push(pattern[1]);
        }
        continue;
      }
      curNode.#children[key] = new _Node2();
      if (pattern) {
        curNode.#patterns.push(pattern);
        possibleKeys.push(pattern[1]);
      }
      curNode = curNode.#children[key];
    }
    curNode.#methods.push({
      [method]: {
        handler,
        possibleKeys: possibleKeys.filter((v, i, a) => a.indexOf(v) === i),
        score: this.#order
      }
    });
    return curNode;
  }
  #getHandlerSets(node, method, nodeParams, params) {
    const handlerSets = [];
    for (let i = 0, len = node.#methods.length; i < len; i++) {
      const m = node.#methods[i];
      const handlerSet = m[method] || m[METHOD_NAME_ALL];
      const processedSet = {};
      if (handlerSet !== void 0) {
        handlerSet.params = /* @__PURE__ */ Object.create(null);
        handlerSets.push(handlerSet);
        if (nodeParams !== emptyParams || params && params !== emptyParams) {
          for (let i2 = 0, len2 = handlerSet.possibleKeys.length; i2 < len2; i2++) {
            const key = handlerSet.possibleKeys[i2];
            const processed = processedSet[handlerSet.score];
            handlerSet.params[key] = params?.[key] && !processed ? params[key] : nodeParams[key] ?? params?.[key];
            processedSet[handlerSet.score] = true;
          }
        }
      }
    }
    return handlerSets;
  }
  search(method, path) {
    const handlerSets = [];
    this.#params = emptyParams;
    const curNode = this;
    let curNodes = [curNode];
    const parts = splitPath(path);
    const curNodesQueue = [];
    for (let i = 0, len = parts.length; i < len; i++) {
      const part = parts[i];
      const isLast = i === len - 1;
      const tempNodes = [];
      for (let j = 0, len2 = curNodes.length; j < len2; j++) {
        const node = curNodes[j];
        const nextNode = node.#children[part];
        if (nextNode) {
          nextNode.#params = node.#params;
          if (isLast) {
            if (nextNode.#children["*"]) {
              handlerSets.push(
                ...this.#getHandlerSets(nextNode.#children["*"], method, node.#params)
              );
            }
            handlerSets.push(...this.#getHandlerSets(nextNode, method, node.#params));
          } else {
            tempNodes.push(nextNode);
          }
        }
        for (let k = 0, len3 = node.#patterns.length; k < len3; k++) {
          const pattern = node.#patterns[k];
          const params = node.#params === emptyParams ? {} : { ...node.#params };
          if (pattern === "*") {
            const astNode = node.#children["*"];
            if (astNode) {
              handlerSets.push(...this.#getHandlerSets(astNode, method, node.#params));
              astNode.#params = params;
              tempNodes.push(astNode);
            }
            continue;
          }
          const [key, name, matcher] = pattern;
          if (!part && !(matcher instanceof RegExp)) {
            continue;
          }
          const child = node.#children[key];
          const restPathString = parts.slice(i).join("/");
          if (matcher instanceof RegExp) {
            const m = matcher.exec(restPathString);
            if (m) {
              params[name] = m[0];
              handlerSets.push(...this.#getHandlerSets(child, method, node.#params, params));
              if (Object.keys(child.#children).length) {
                child.#params = params;
                const componentCount = m[0].match(/\//)?.length ?? 0;
                const targetCurNodes = curNodesQueue[componentCount] ||= [];
                targetCurNodes.push(child);
              }
              continue;
            }
          }
          if (matcher === true || matcher.test(part)) {
            params[name] = part;
            if (isLast) {
              handlerSets.push(...this.#getHandlerSets(child, method, params, node.#params));
              if (child.#children["*"]) {
                handlerSets.push(
                  ...this.#getHandlerSets(child.#children["*"], method, params, node.#params)
                );
              }
            } else {
              child.#params = params;
              tempNodes.push(child);
            }
          }
        }
      }
      curNodes = tempNodes.concat(curNodesQueue.shift() ?? []);
    }
    if (handlerSets.length > 1) {
      handlerSets.sort((a, b) => {
        return a.score - b.score;
      });
    }
    return [handlerSets.map(({ handler, params }) => [handler, params])];
  }
};

// node_modules/hono/dist/router/trie-router/router.js
var TrieRouter = class {
  static {
    __name(this, "TrieRouter");
  }
  name = "TrieRouter";
  #node;
  constructor() {
    this.#node = new Node2();
  }
  add(method, path, handler) {
    const results = checkOptionalParameter(path);
    if (results) {
      for (let i = 0, len = results.length; i < len; i++) {
        this.#node.insert(method, results[i], handler);
      }
      return;
    }
    this.#node.insert(method, path, handler);
  }
  match(method, path) {
    return this.#node.search(method, path);
  }
};

// node_modules/hono/dist/hono.js
var Hono2 = class extends Hono {
  static {
    __name(this, "Hono");
  }
  /**
   * Creates an instance of the Hono class.
   *
   * @param options - Optional configuration options for the Hono instance.
   */
  constructor(options = {}) {
    super(options);
    this.router = options.router ?? new SmartRouter({
      routers: [new RegExpRouter(), new TrieRouter()]
    });
  }
};

// node_modules/hono/dist/middleware/cors/index.js
var cors = /* @__PURE__ */ __name((options) => {
  const defaults = {
    origin: "*",
    allowMethods: ["GET", "HEAD", "PUT", "POST", "DELETE", "PATCH"],
    allowHeaders: [],
    exposeHeaders: []
  };
  const opts = {
    ...defaults,
    ...options
  };
  const findAllowOrigin = ((optsOrigin) => {
    if (typeof optsOrigin === "string") {
      if (optsOrigin === "*") {
        return () => optsOrigin;
      } else {
        return (origin) => optsOrigin === origin ? origin : null;
      }
    } else if (typeof optsOrigin === "function") {
      return optsOrigin;
    } else {
      return (origin) => optsOrigin.includes(origin) ? origin : null;
    }
  })(opts.origin);
  const findAllowMethods = ((optsAllowMethods) => {
    if (typeof optsAllowMethods === "function") {
      return optsAllowMethods;
    } else if (Array.isArray(optsAllowMethods)) {
      return () => optsAllowMethods;
    } else {
      return () => [];
    }
  })(opts.allowMethods);
  return /* @__PURE__ */ __name(async function cors2(c, next) {
    function set(key, value) {
      c.res.headers.set(key, value);
    }
    __name(set, "set");
    const allowOrigin = await findAllowOrigin(c.req.header("origin") || "", c);
    if (allowOrigin) {
      set("Access-Control-Allow-Origin", allowOrigin);
    }
    if (opts.credentials) {
      set("Access-Control-Allow-Credentials", "true");
    }
    if (opts.exposeHeaders?.length) {
      set("Access-Control-Expose-Headers", opts.exposeHeaders.join(","));
    }
    if (c.req.method === "OPTIONS") {
      if (opts.origin !== "*") {
        set("Vary", "Origin");
      }
      if (opts.maxAge != null) {
        set("Access-Control-Max-Age", opts.maxAge.toString());
      }
      const allowMethods = await findAllowMethods(c.req.header("origin") || "", c);
      if (allowMethods.length) {
        set("Access-Control-Allow-Methods", allowMethods.join(","));
      }
      let headers = opts.allowHeaders;
      if (!headers?.length) {
        const requestHeaders = c.req.header("Access-Control-Request-Headers");
        if (requestHeaders) {
          headers = requestHeaders.split(/\s*,\s*/);
        }
      }
      if (headers?.length) {
        set("Access-Control-Allow-Headers", headers.join(","));
        c.res.headers.append("Vary", "Access-Control-Request-Headers");
      }
      c.res.headers.delete("Content-Length");
      c.res.headers.delete("Content-Type");
      return new Response(null, {
        headers: c.res.headers,
        status: 204,
        statusText: "No Content"
      });
    }
    await next();
    if (opts.origin !== "*") {
      c.header("Vary", "Origin", { append: true });
    }
  }, "cors2");
}, "cors");

// src/lib/utils.ts
var encoder = new TextEncoder();
function randomId(prefix) {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  const value = Array.from(bytes, (value2) => value2.toString(16).padStart(2, "0")).join("");
  return `${prefix}_${value}`;
}
__name(randomId, "randomId");
function randomCode(length = 24) {
  const bytes = new Uint8Array(length);
  crypto.getRandomValues(bytes);
  return btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}
__name(randomCode, "randomCode");
async function sha256(value) {
  const hash = await crypto.subtle.digest("SHA-256", encoder.encode(value));
  const bytes = new Uint8Array(hash);
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}
__name(sha256, "sha256");
function addSeconds(date, seconds) {
  return new Date(date.getTime() + seconds * 1e3);
}
__name(addSeconds, "addSeconds");
function toIso(date) {
  return date.toISOString();
}
__name(toIso, "toIso");
function parsePositiveInt(value, fallback) {
  if (!value) return fallback;
  const parsed = Number.parseInt(value, 10);
  if (Number.isNaN(parsed) || parsed <= 0) return fallback;
  return parsed;
}
__name(parsePositiveInt, "parsePositiveInt");
function isEmail(value) {
  return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value);
}
__name(isEmail, "isEmail");
function getCookie(cookieHeader, name) {
  if (!cookieHeader) return null;
  const parts = cookieHeader.split(";").map((part) => part.trim());
  for (const part of parts) {
    const [key, ...rest] = part.split("=");
    if (key === name) return rest.join("=");
  }
  return null;
}
__name(getCookie, "getCookie");
function buildSessionCookie(name, value, expiresAt) {
  return `${name}=${value}; Path=/; HttpOnly; Secure; SameSite=Lax; Expires=${expiresAt.toUTCString()}`;
}
__name(buildSessionCookie, "buildSessionCookie");
function clearSessionCookie(name) {
  return `${name}=; Path=/; HttpOnly; Secure; SameSite=Lax; Expires=Thu, 01 Jan 1970 00:00:00 GMT`;
}
__name(clearSessionCookie, "clearSessionCookie");

// src/lib/db.ts
async function findUserByEmail(db, email) {
  const result = await db.prepare("SELECT id, email, role FROM users WHERE email = ?").bind(email).first();
  return result ?? null;
}
__name(findUserByEmail, "findUserByEmail");
async function findUserById(db, userId) {
  const result = await db.prepare("SELECT id, email, role FROM users WHERE id = ?").bind(userId).first();
  return result ?? null;
}
__name(findUserById, "findUserById");
async function createUser(db, email) {
  const userId = randomId("usr");
  await db.prepare("INSERT INTO users (id, email, role, created_at, updated_at) VALUES (?, ?, 'user', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)").bind(userId, email).run();
  return { id: userId, email, role: "user" };
}
__name(createUser, "createUser");
async function upsertUserByEmail(db, email) {
  const existing = await findUserByEmail(db, email);
  if (existing) return existing;
  return createUser(db, email);
}
__name(upsertUserByEmail, "upsertUserByEmail");
async function insertAuthToken(db, email, tokenHash, expiresAt) {
  const id = randomId("atk");
  await db.prepare("INSERT INTO auth_tokens (id, email, token_hash, purpose, expires_at, created_at) VALUES (?, ?, ?, 'login', ?, CURRENT_TIMESTAMP)").bind(id, email, tokenHash, toIso(expiresAt)).run();
  return id;
}
__name(insertAuthToken, "insertAuthToken");
async function useAuthToken(db, email, tokenHash, nowIso) {
  const token = await db.prepare(
    "SELECT id FROM auth_tokens WHERE email = ? AND token_hash = ? AND used_at IS NULL AND expires_at > ? ORDER BY created_at DESC LIMIT 1"
  ).bind(email, tokenHash, nowIso).first();
  if (!token) return false;
  await db.prepare("UPDATE auth_tokens SET used_at = CURRENT_TIMESTAMP WHERE id = ? AND used_at IS NULL").bind(token.id).run();
  return true;
}
__name(useAuthToken, "useAuthToken");
async function createSession(db, userId, expiresAt) {
  const sessionId = randomId("ses");
  await db.prepare(
    "INSERT INTO sessions (id, user_id, expires_at, created_at, last_seen_at) VALUES (?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)"
  ).bind(sessionId, userId, toIso(expiresAt)).run();
  return sessionId;
}
__name(createSession, "createSession");
async function findSession(db, sessionId) {
  const session = await db.prepare("SELECT id, user_id, expires_at, revoked_at FROM sessions WHERE id = ?").bind(sessionId).first();
  return session ?? null;
}
__name(findSession, "findSession");
async function revokeSession(db, sessionId) {
  await db.prepare("UPDATE sessions SET revoked_at = CURRENT_TIMESTAMP WHERE id = ?").bind(sessionId).run();
}
__name(revokeSession, "revokeSession");
async function touchSession(db, sessionId) {
  await db.prepare("UPDATE sessions SET last_seen_at = CURRENT_TIMESTAMP WHERE id = ?").bind(sessionId).run();
}
__name(touchSession, "touchSession");

// src/lib/email.ts
function escapeHtml(value) {
  return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\"/g, "&quot;").replace(/'/g, "&#39;");
}
__name(escapeHtml, "escapeHtml");
async function sendMagicCodeEmail(env, recipientEmail, code, expiresInMinutes) {
  const isProduction = env.ENVIRONMENT === "production";
  if (!env.SENDGRID_API_KEY) {
    if (isProduction) {
      throw new Error("SENDGRID_API_KEY is required in production");
    }
    console.warn(
      `[auth] SENDGRID_API_KEY is not set. Dev fallback active. email=${recipientEmail} code=${code} expiresInMinutes=${expiresInMinutes}`
    );
    return;
  }
  const subject = "Your Soundshed sign-in code";
  const safeCode = escapeHtml(code);
  const html = `
    <div style="font-family:Arial,sans-serif;max-width:560px;margin:0 auto;padding:16px;">
      <h2 style="margin:0 0 12px;">Sign in to Soundshed</h2>
      <p style="margin:0 0 12px;">Use this one-time code:</p>
      <div style="font-size:24px;font-weight:700;letter-spacing:2px;margin:12px 0 16px;">${safeCode}</div>
      <p style="margin:0 0 8px;">This code expires in ${expiresInMinutes} minutes.</p>
      <p style="margin:0;color:#666;">If you did not request this code, you can ignore this email.</p>
    </div>
  `;
  const text = [
    "Sign in to Soundshed",
    "",
    `Your one-time code: ${code}`,
    `Expires in ${expiresInMinutes} minutes.`,
    "",
    "If you did not request this code, you can ignore this email."
  ].join("\n");
  const response = await fetch("https://api.sendgrid.com/v3/mail/send", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.SENDGRID_API_KEY}`,
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      personalizations: [
        {
          to: [{ email: recipientEmail }]
        }
      ],
      from: {
        email: env.SENDGRID_FROM_EMAIL,
        name: env.SENDGRID_FROM_NAME
      },
      subject,
      content: [
        { type: "text/plain", value: text },
        { type: "text/html", value: html }
      ]
    })
  });
  if (!response.ok) {
    const errorBody = await response.text();
    throw new Error(`SendGrid failed: ${response.status} ${errorBody}`);
  }
}
__name(sendMagicCodeEmail, "sendMagicCodeEmail");

// src/lib/http.ts
function ok(c, data, status = 200) {
  return c.newResponse(JSON.stringify({ ok: true, data }), status, {
    "content-type": "application/json"
  });
}
__name(ok, "ok");
function fail(c, code, message, status = 400) {
  return c.newResponse(JSON.stringify({ ok: false, error: { code, message } }), status, {
    "content-type": "application/json"
  });
}
__name(fail, "fail");
async function safeJson(request) {
  try {
    return await request.json();
  } catch {
    return null;
  }
}
__name(safeJson, "safeJson");

// src/middleware/session.ts
var optionalAuth = /* @__PURE__ */ __name(async (c, next) => {
  const cookieName = c.env.COOKIE_NAME;
  const headerSessionId = c.req.header("x-session-id")?.trim() ?? "";
  const cookieSessionId = getCookie(c.req.header("cookie") ?? null, cookieName) ?? "";
  const sessionId = headerSessionId || cookieSessionId;
  if (!sessionId) {
    await next();
    return;
  }
  const session = await findSession(c.env.DB, sessionId);
  if (!session || session.revoked_at) {
    await next();
    return;
  }
  if (new Date(session.expires_at).getTime() <= Date.now()) {
    await next();
    return;
  }
  const user = await findUserById(c.env.DB, session.user_id);
  if (!user) {
    await next();
    return;
  }
  await touchSession(c.env.DB, session.id);
  c.set("auth", { userId: user.id, email: user.email, role: user.role, sessionId: session.id });
  await next();
}, "optionalAuth");
var requireAuth = /* @__PURE__ */ __name(async (c, next) => {
  await optionalAuth(c, async () => void 0);
  const auth = c.get("auth");
  if (!auth) {
    c.res = fail(c, "UNAUTHORIZED", "Authentication required", 401);
    return;
  }
  await next();
}, "requireAuth");

// src/routes/auth.ts
function authRoutes() {
  const app2 = new Hono2();
  app2.post("/start", async (c) => {
    const body = await safeJson(c.req.raw);
    const email = body?.email?.trim().toLowerCase();
    if (!email || !isEmail(email)) {
      return fail(c, "INVALID_EMAIL", "A valid email is required", 422);
    }
    const code = randomCode(18);
    const tokenHash = await sha256(code);
    const ttl = parsePositiveInt(c.env.MAGIC_LINK_TTL_SECONDS, 900);
    const expiresAt = addSeconds(/* @__PURE__ */ new Date(), ttl);
    await insertAuthToken(c.env.DB, email, tokenHash, expiresAt);
    const expiresInMinutes = Math.max(1, Math.floor(ttl / 60));
    try {
      await sendMagicCodeEmail(c.env, email, code, expiresInMinutes);
    } catch (error) {
      return fail(c, "EMAIL_SEND_FAILED", error instanceof Error ? error.message : "Could not send sign-in email", 502);
    }
    return ok(c, {
      email,
      expiresAt: expiresAt.toISOString()
    });
  });
  app2.post("/verify", async (c) => {
    const body = await safeJson(c.req.raw);
    const email = body?.email?.trim().toLowerCase();
    const code = body?.code?.trim();
    if (!email || !isEmail(email) || !code) {
      return fail(c, "INVALID_REQUEST", "email and code are required", 422);
    }
    const tokenHash = await sha256(code);
    const isValid = await useAuthToken(c.env.DB, email, tokenHash, (/* @__PURE__ */ new Date()).toISOString());
    if (!isValid) {
      return fail(c, "INVALID_TOKEN", "Code is invalid or expired", 401);
    }
    const user = await upsertUserByEmail(c.env.DB, email);
    const sessionTtl = parsePositiveInt(c.env.SESSION_TTL_SECONDS, 2592e3);
    const sessionExpiresAt = addSeconds(/* @__PURE__ */ new Date(), sessionTtl);
    const sessionId = await createSession(c.env.DB, user.id, sessionExpiresAt);
    c.header("Set-Cookie", buildSessionCookie(c.env.COOKIE_NAME, sessionId, sessionExpiresAt));
    return ok(c, {
      sessionId,
      user: {
        id: user.id,
        email: user.email,
        role: user.role
      }
    });
  });
  app2.get("/me", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return ok(c, { user: null });
    }
    return ok(c, {
      user: {
        id: auth.userId,
        email: auth.email,
        role: auth.role
      }
    });
  });
  app2.post("/logout", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (auth?.sessionId) {
      await revokeSession(c.env.DB, auth.sessionId);
    }
    c.header("Set-Cookie", clearSessionCookie(c.env.COOKIE_NAME));
    return ok(c, { loggedOut: true });
  });
  return app2;
}
__name(authRoutes, "authRoutes");

// src/routes/discovery.ts
function discoveryRoutes() {
  const app2 = new Hono2();
  app2.get("/home", async (c) => {
    const cacheKey = "home:v1";
    const cached = await c.env.DISCOVERY_CACHE.get(cacheKey, { type: "json" });
    if (cached) {
      return ok(c, cached);
    }
    const rows = await c.env.DB.prepare(
      "SELECT id, slug, title, sort_order FROM featured_rows WHERE active = 1 ORDER BY sort_order ASC LIMIT 12"
    ).all();
    const resultRows = [];
    for (const row of rows.results) {
      const rowItems = await c.env.DB.prepare(
        `SELECT fri.item_id, fri.pack_id, i.title AS item_title, i.type AS item_type, p.title AS pack_title
         FROM featured_row_items fri
         LEFT JOIN items i ON i.id = fri.item_id
         LEFT JOIN packs p ON p.id = fri.pack_id
         WHERE fri.row_id = ?
         ORDER BY fri.sort_order ASC
         LIMIT 40`
      ).bind(row.id).all();
      const mappedItems = rowItems.results.map((entry) => {
        if (entry.item_id) {
          return {
            id: entry.item_id,
            kind: "item",
            title: entry.item_title ?? "Untitled",
            type: entry.item_type
          };
        }
        return {
          id: entry.pack_id ?? "",
          kind: "pack",
          title: entry.pack_title ?? "Untitled Pack",
          type: null
        };
      });
      resultRows.push({
        id: row.id,
        slug: row.slug,
        title: row.title,
        items: mappedItems
      });
    }
    if (resultRows.length === 0) {
      const latestItems = await c.env.DB.prepare(
        `SELECT id, title, type
           FROM items
           WHERE moderation_status = 'approved'
           ORDER BY published_at DESC, updated_at DESC
           LIMIT 40`
      ).all();
      const latestPacks = await c.env.DB.prepare(
        `SELECT id, title
           FROM packs
           WHERE moderation_status = 'approved'
           ORDER BY published_at DESC, updated_at DESC
           LIMIT 20`
      ).all();
      if (latestItems.results.length > 0) {
        resultRows.push({
          id: "fallback_latest_items",
          slug: "latest-items",
          title: "Latest Presets",
          items: latestItems.results.map((item) => ({
            id: item.id,
            kind: "item",
            title: item.title,
            type: item.type
          }))
        });
      }
      if (latestPacks.results.length > 0) {
        resultRows.push({
          id: "fallback_latest_packs",
          slug: "latest-packs",
          title: "Latest Packs",
          items: latestPacks.results.map((pack) => ({
            id: pack.id,
            kind: "pack",
            title: pack.title,
            type: null
          }))
        });
      }
    }
    const payload = {
      generatedAt: (/* @__PURE__ */ new Date()).toISOString(),
      rows: resultRows
    };
    await c.env.DISCOVERY_CACHE.put(cacheKey, JSON.stringify(payload), { expirationTtl: 120 });
    return ok(c, payload);
  });
  app2.get("/search", async (c) => {
    const query = (c.req.query("q") ?? "").trim();
    const type = (c.req.query("type") ?? "").trim();
    const taxonomy = (c.req.query("taxonomy") ?? "").trim();
    const params = [];
    let sql = `
      SELECT DISTINCT i.id, i.title, i.type, i.moderation_status, i.published_at
      FROM items i
      LEFT JOIN item_taxonomies it ON it.item_id = i.id
      LEFT JOIN taxonomies t ON t.id = it.taxonomy_id
      WHERE i.moderation_status = 'approved'
    `;
    if (query.length > 0) {
      sql += " AND i.title LIKE ?";
      params.push(`%${query}%`);
    }
    if (type.length > 0) {
      sql += " AND i.type = ?";
      params.push(type);
    }
    if (taxonomy.length > 0) {
      sql += " AND t.slug = ?";
      params.push(taxonomy);
    }
    sql += " ORDER BY i.published_at DESC LIMIT 100";
    const statement = c.env.DB.prepare(sql).bind(...params);
    const items = await statement.all();
    return ok(c, { items: items.results });
  });
  app2.get("/rows/:slug", async (c) => {
    const slug = c.req.param("slug");
    const row = await c.env.DB.prepare("SELECT id, slug, title FROM featured_rows WHERE slug = ? AND active = 1").bind(slug).first();
    if (!row) {
      return ok(c, { row: null, items: [] });
    }
    const rowItems = await c.env.DB.prepare(
      `SELECT fri.item_id, fri.pack_id, fri.sort_order, i.title AS item_title, i.type AS item_type, p.title AS pack_title
         FROM featured_row_items fri
         LEFT JOIN items i ON i.id = fri.item_id
         LEFT JOIN packs p ON p.id = fri.pack_id
         WHERE fri.row_id = ?
         ORDER BY fri.sort_order ASC
         LIMIT 200`
    ).bind(row.id).all();
    return ok(c, {
      row: {
        id: row.id,
        slug: row.slug,
        title: row.title
      },
      items: rowItems.results.map((entry) => ({
        id: entry.item_id ?? entry.pack_id ?? "",
        kind: entry.item_id ? "item" : "pack",
        sortOrder: entry.sort_order,
        title: entry.item_id ? entry.item_title ?? "Untitled" : entry.pack_title ?? "Untitled Pack",
        type: entry.item_type
      }))
    });
  });
  return app2;
}
__name(discoveryRoutes, "discoveryRoutes");

// src/routes/health.ts
function healthRoutes() {
  const app2 = new Hono2();
  app2.get("/health", async (c) => {
    const probe = await c.env.DB.prepare("SELECT 1 AS ok").first();
    return ok(c, {
      status: "ok",
      db: probe?.ok === 1 ? "ok" : "error",
      now: (/* @__PURE__ */ new Date()).toISOString()
    });
  });
  return app2;
}
__name(healthRoutes, "healthRoutes");

// src/routes/items.ts
var allowedTypes = /* @__PURE__ */ new Set(["preset", "blend", "layout", "composite", "combo"]);
var allowedVisibility = /* @__PURE__ */ new Set(["public", "unlisted", "private"]);
function parseItemConfig(configJson) {
  const defaults = {
    description: null,
    visibility: "public",
    appMinVersion: null,
    appMaxVersion: null,
    payloadAssetId: null,
    manifestAssetId: null,
    thumbnailAssetId: null,
    previewAssetId: null
  };
  if (!configJson) {
    return defaults;
  }
  try {
    const parsed = JSON.parse(configJson);
    const visibility = parsed.visibility;
    return {
      description: typeof parsed.description === "string" ? parsed.description : null,
      visibility: visibility && allowedVisibility.has(visibility) ? visibility : "public",
      appMinVersion: typeof parsed.appMinVersion === "string" ? parsed.appMinVersion : null,
      appMaxVersion: typeof parsed.appMaxVersion === "string" ? parsed.appMaxVersion : null,
      payloadAssetId: typeof parsed.payloadAssetId === "string" ? parsed.payloadAssetId : null,
      manifestAssetId: typeof parsed.manifestAssetId === "string" ? parsed.manifestAssetId : null,
      thumbnailAssetId: typeof parsed.thumbnailAssetId === "string" ? parsed.thumbnailAssetId : null,
      previewAssetId: typeof parsed.previewAssetId === "string" ? parsed.previewAssetId : null
    };
  } catch {
    return defaults;
  }
}
__name(parseItemConfig, "parseItemConfig");
function stringifyItemConfig(config) {
  return JSON.stringify(config);
}
__name(stringifyItemConfig, "stringifyItemConfig");
function toItemResponse(item) {
  const config = parseItemConfig(item.config_json);
  return {
    id: item.id,
    creatorUserId: item.creator_user_id,
    type: item.type,
    title: item.title,
    description: config.description,
    visibility: config.visibility,
    moderationStatus: item.moderation_status,
    appMinVersion: config.appMinVersion,
    appMaxVersion: config.appMaxVersion,
    payloadAssetId: config.payloadAssetId,
    manifestAssetId: config.manifestAssetId,
    thumbnailAssetId: config.thumbnailAssetId,
    previewAssetId: config.previewAssetId,
    publishedAt: item.published_at,
    createdAt: item.created_at,
    updatedAt: item.updated_at
  };
}
__name(toItemResponse, "toItemResponse");
function downloadFileName(base, ext) {
  const normalized = base.trim().toLowerCase().replace(/[^a-z0-9\-_ ]/g, "").replace(/\s+/g, "-").slice(0, 80);
  const safe = normalized.length > 0 ? normalized : "preset";
  return `${safe}.${ext}`;
}
__name(downloadFileName, "downloadFileName");
function itemRoutes() {
  const app2 = new Hono2();
  app2.get("/", async (c) => {
    const page = Math.max(1, Number.parseInt((c.req.query("page") ?? "1").trim(), 10) || 1);
    const pageSizeRaw = Number.parseInt((c.req.query("pageSize") ?? "24").trim(), 10) || 24;
    const pageSize = Math.min(100, Math.max(1, pageSizeRaw));
    const offset = (page - 1) * pageSize;
    const type = (c.req.query("type") ?? "").trim();
    const query = (c.req.query("q") ?? "").trim();
    const taxonomy = (c.req.query("taxonomy") ?? "").trim();
    const params = [];
    let sql = `
      SELECT DISTINCT i.id, i.creator_user_id, i.type, i.title, i.moderation_status, i.config_json,
             i.published_at, i.created_at, i.updated_at
      FROM items i
      LEFT JOIN item_taxonomies it ON it.item_id = i.id
      LEFT JOIN taxonomies t ON t.id = it.taxonomy_id
      WHERE i.moderation_status = 'approved'
    `;
    if (type.length > 0) {
      sql += " AND i.type = ?";
      params.push(type);
    }
    if (query.length > 0) {
      sql += " AND i.title LIKE ?";
      params.push(`%${query}%`);
    }
    if (taxonomy.length > 0) {
      sql += " AND t.slug = ?";
      params.push(taxonomy);
    }
    sql += " ORDER BY i.published_at DESC, i.updated_at DESC LIMIT ? OFFSET ?";
    params.push(pageSize, offset);
    const rows = await c.env.DB.prepare(sql).bind(...params).all();
    return ok(c, {
      page,
      pageSize,
      items: rows.results.map(toItemResponse)
    });
  });
  app2.get("/me/list", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return ok(c, { items: [] });
    }
    const status = (c.req.query("status") ?? "").trim();
    const type = (c.req.query("type") ?? "").trim();
    const params = [auth.userId];
    let sql = `
      SELECT id, creator_user_id, type, title, moderation_status, config_json,
             published_at, created_at, updated_at
      FROM items
      WHERE creator_user_id = ?
    `;
    if (status.length > 0) {
      sql += " AND moderation_status = ?";
      params.push(status);
    }
    if (type.length > 0) {
      sql += " AND type = ?";
      params.push(type);
    }
    sql += " ORDER BY updated_at DESC LIMIT 200";
    const rows = await c.env.DB.prepare(sql).bind(...params).all();
    return ok(c, { items: rows.results.map(toItemResponse) });
  });
  app2.post("/", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const body = await safeJson(c.req.raw);
    const type = body?.type;
    const title = body?.title?.trim();
    const visibility = body?.visibility ?? "public";
    if (!type || !allowedTypes.has(type)) {
      return fail(c, "INVALID_TYPE", "Invalid item type", 422);
    }
    if (!title) {
      return fail(c, "INVALID_TITLE", "title is required", 422);
    }
    if (!allowedVisibility.has(visibility)) {
      return fail(c, "INVALID_VISIBILITY", "Invalid visibility", 422);
    }
    const itemId = randomId("itm");
    const config = {
      description: body?.description?.trim() ?? null,
      visibility,
      appMinVersion: body?.appMinVersion?.trim() ?? null,
      appMaxVersion: body?.appMaxVersion?.trim() ?? null,
      payloadAssetId: body?.payloadAssetId?.trim() ?? null,
      manifestAssetId: body?.manifestAssetId?.trim() ?? null,
      thumbnailAssetId: body?.thumbnailAssetId?.trim() ?? null,
      previewAssetId: body?.previewAssetId?.trim() ?? null
    };
    await c.env.DB.prepare(
      `INSERT INTO items (
          id, creator_user_id, type, title, moderation_status, config_json,
          created_at, updated_at
        ) VALUES (?, ?, ?, ?, 'draft', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)`
    ).bind(
      itemId,
      auth.userId,
      type,
      title,
      stringifyItemConfig(config)
    ).run();
    const created = await c.env.DB.prepare(
      `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
    ).bind(itemId).first();
    return ok(c, { item: created ? toItemResponse(created) : null }, 201);
  });
  app2.patch("/:itemId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const itemId = c.req.param("itemId");
    const existing = await c.env.DB.prepare(
      `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
    ).bind(itemId).first();
    if (!existing) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (existing.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }
    const body = await safeJson(c.req.raw);
    if (!body) {
      return fail(c, "INVALID_BODY", "Invalid request body", 422);
    }
    const type = body.type ?? existing.type;
    const title = body.title !== void 0 ? body.title.trim() : existing.title;
    const existingConfig = parseItemConfig(existing.config_json);
    const visibility = body.visibility ?? existingConfig.visibility;
    if (!allowedTypes.has(type)) {
      return fail(c, "INVALID_TYPE", "Invalid item type", 422);
    }
    if (!title) {
      return fail(c, "INVALID_TITLE", "title cannot be empty", 422);
    }
    if (!allowedVisibility.has(visibility)) {
      return fail(c, "INVALID_VISIBILITY", "Invalid visibility", 422);
    }
    const nextConfig = {
      description: body.description !== void 0 ? body.description?.trim() ?? null : existingConfig.description,
      visibility,
      appMinVersion: body.appMinVersion !== void 0 ? body.appMinVersion?.trim() ?? null : existingConfig.appMinVersion,
      appMaxVersion: body.appMaxVersion !== void 0 ? body.appMaxVersion?.trim() ?? null : existingConfig.appMaxVersion,
      payloadAssetId: body.payloadAssetId !== void 0 ? body.payloadAssetId?.trim() ?? null : existingConfig.payloadAssetId,
      manifestAssetId: body.manifestAssetId !== void 0 ? body.manifestAssetId?.trim() ?? null : existingConfig.manifestAssetId,
      thumbnailAssetId: body.thumbnailAssetId !== void 0 ? body.thumbnailAssetId?.trim() ?? null : existingConfig.thumbnailAssetId,
      previewAssetId: body.previewAssetId !== void 0 ? body.previewAssetId?.trim() ?? null : existingConfig.previewAssetId
    };
    await c.env.DB.prepare(
      `UPDATE items SET
          type = ?,
          title = ?,
          config_json = ?,
          updated_at = CURRENT_TIMESTAMP
         WHERE id = ?`
    ).bind(
      type,
      title,
      stringifyItemConfig(nextConfig),
      itemId
    ).run();
    const updated = await c.env.DB.prepare(
      `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
    ).bind(itemId).first();
    return ok(c, { item: updated ? toItemResponse(updated) : null });
  });
  app2.post("/:itemId/submit", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const itemId = c.req.param("itemId");
    const item = await c.env.DB.prepare("SELECT id, creator_user_id, moderation_status FROM items WHERE id = ?").bind(itemId).first();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }
    await c.env.DB.prepare("UPDATE items SET moderation_status = 'pending_review', updated_at = CURRENT_TIMESTAMP WHERE id = ?").bind(itemId).run();
    return ok(c, { itemId, moderationStatus: "pending_review" });
  });
  app2.post("/:itemId/publish", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const itemId = c.req.param("itemId");
    const item = await c.env.DB.prepare("SELECT id, creator_user_id, config_json FROM items WHERE id = ?").bind(itemId).first();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }
    const config = parseItemConfig(item.config_json);
    if (!config.payloadAssetId) {
      return fail(c, "MISSING_PAYLOAD", "Cannot publish without payloadAssetId", 422);
    }
    await c.env.DB.prepare(
      "UPDATE items SET moderation_status = 'approved', published_at = COALESCE(published_at, CURRENT_TIMESTAMP), updated_at = CURRENT_TIMESTAMP WHERE id = ?"
    ).bind(itemId).run();
    return ok(c, { itemId, moderationStatus: "approved" });
  });
  app2.get("/:itemId", optionalAuth, async (c) => {
    const itemId = c.req.param("itemId");
    const auth = c.get("auth");
    const item = await c.env.DB.prepare(
      `SELECT id, creator_user_id, type, title, moderation_status, config_json,
                published_at, created_at, updated_at
         FROM items WHERE id = ?`
    ).bind(itemId).first();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    const isOwner = auth?.userId === item.creator_user_id;
    const isPublic = item.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    return ok(c, { item: toItemResponse(item) });
  });
  app2.get("/:itemId/download", optionalAuth, async (c) => {
    const itemId = c.req.param("itemId");
    const auth = c.get("auth");
    const item = await c.env.DB.prepare("SELECT id, creator_user_id, title, moderation_status, config_json FROM items WHERE id = ?").bind(itemId).first();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    const isOwner = auth?.userId === item.creator_user_id;
    const isPublic = item.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    const config = parseItemConfig(item.config_json);
    if (!config.payloadAssetId) {
      return fail(c, "MISSING_PAYLOAD", "Item payload is not available", 409);
    }
    const asset = await c.env.DB.prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?").bind(config.payloadAssetId).first();
    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Payload asset not found", 404);
    }
    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Payload object not found", 404);
    }
    await c.env.DB.prepare("INSERT INTO downloads (id, user_id, item_id, pack_id, created_at) VALUES (?, ?, ?, NULL, CURRENT_TIMESTAMP)").bind(randomId("dwl"), auth?.userId ?? null, itemId).run();
    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/octet-stream";
    const fileName = downloadFileName(item.title, "preset");
    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType,
        "content-disposition": `attachment; filename="${fileName}"`
      }
    });
  });
  app2.delete("/:itemId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const itemId = c.req.param("itemId");
    const item = await c.env.DB.prepare("SELECT id, creator_user_id FROM items WHERE id = ?").bind(itemId).first();
    if (!item) {
      return fail(c, "NOT_FOUND", "Item not found", 404);
    }
    if (item.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this item", 403);
    }
    await c.env.DB.prepare("DELETE FROM pack_items WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM featured_row_items WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM favorites WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM ratings WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM item_taxonomies WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM downloads WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM reports WHERE item_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM moderation_actions WHERE target_type = 'item' AND target_id = ?").bind(itemId).run();
    await c.env.DB.prepare("DELETE FROM items WHERE id = ?").bind(itemId).run();
    return ok(c, { itemId, deleted: true });
  });
  return app2;
}
__name(itemRoutes, "itemRoutes");

// src/routes/packs.ts
function parsePackConfig(configJson) {
  const defaults = {
    description: null,
    zipAssetId: null,
    thumbnailAssetId: null
  };
  if (!configJson) {
    return defaults;
  }
  try {
    const parsed = JSON.parse(configJson);
    return {
      description: typeof parsed.description === "string" ? parsed.description : null,
      zipAssetId: typeof parsed.zipAssetId === "string" ? parsed.zipAssetId : null,
      thumbnailAssetId: typeof parsed.thumbnailAssetId === "string" ? parsed.thumbnailAssetId : null
    };
  } catch {
    return defaults;
  }
}
__name(parsePackConfig, "parsePackConfig");
function stringifyPackConfig(config) {
  return JSON.stringify(config);
}
__name(stringifyPackConfig, "stringifyPackConfig");
function toPackResponse(pack) {
  const config = parsePackConfig(pack.config_json);
  return {
    id: pack.id,
    creatorUserId: pack.creator_user_id,
    title: pack.title,
    description: config.description,
    moderationStatus: pack.moderation_status,
    zipAssetId: config.zipAssetId,
    thumbnailAssetId: config.thumbnailAssetId,
    publishedAt: pack.published_at,
    createdAt: pack.created_at,
    updatedAt: pack.updated_at
  };
}
__name(toPackResponse, "toPackResponse");
function downloadFileName2(base, ext) {
  const normalized = base.trim().toLowerCase().replace(/[^a-z0-9\-_ ]/g, "").replace(/\s+/g, "-").slice(0, 80);
  const safe = normalized.length > 0 ? normalized : "pack";
  return `${safe}.${ext}`;
}
__name(downloadFileName2, "downloadFileName");
function packRoutes() {
  const app2 = new Hono2();
  app2.get("/", async (c) => {
    const page = Math.max(1, Number.parseInt((c.req.query("page") ?? "1").trim(), 10) || 1);
    const pageSizeRaw = Number.parseInt((c.req.query("pageSize") ?? "24").trim(), 10) || 24;
    const pageSize = Math.min(100, Math.max(1, pageSizeRaw));
    const offset = (page - 1) * pageSize;
    const query = (c.req.query("q") ?? "").trim();
    const params = [];
    let sql = `
          SELECT id, creator_user_id, title, moderation_status,
            config_json, published_at, created_at, updated_at
      FROM packs
      WHERE moderation_status = 'approved'
    `;
    if (query.length > 0) {
      sql += " AND title LIKE ?";
      params.push(`%${query}%`);
    }
    sql += " ORDER BY published_at DESC, updated_at DESC LIMIT ? OFFSET ?";
    params.push(pageSize, offset);
    const rows = await c.env.DB.prepare(sql).bind(...params).all();
    return ok(c, {
      page,
      pageSize,
      packs: rows.results.map(toPackResponse)
    });
  });
  app2.get("/me/list", optionalAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return ok(c, { packs: [] });
    }
    const status = (c.req.query("status") ?? "").trim();
    const params = [auth.userId];
    let sql = `
          SELECT id, creator_user_id, title, moderation_status,
            config_json, published_at, created_at, updated_at
      FROM packs
      WHERE creator_user_id = ?
    `;
    if (status.length > 0) {
      sql += " AND moderation_status = ?";
      params.push(status);
    }
    sql += " ORDER BY updated_at DESC LIMIT 200";
    const rows = await c.env.DB.prepare(sql).bind(...params).all();
    return ok(c, { packs: rows.results.map(toPackResponse) });
  });
  app2.post("/", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const body = await safeJson(c.req.raw);
    const title = body?.title?.trim();
    if (!title) {
      return fail(c, "INVALID_TITLE", "title is required", 422);
    }
    const packId = randomId("pak");
    const config = {
      description: body?.description?.trim() ?? null,
      zipAssetId: body?.zipAssetId?.trim() ?? null,
      thumbnailAssetId: body?.thumbnailAssetId?.trim() ?? null
    };
    await c.env.DB.prepare(
      `INSERT INTO packs (
          id, creator_user_id, title, moderation_status,
          config_json, created_at, updated_at
        ) VALUES (?, ?, ?, 'draft', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)`
    ).bind(packId, auth.userId, title, stringifyPackConfig(config)).run();
    const created = await c.env.DB.prepare(
      `SELECT id, creator_user_id, title, moderation_status,
          config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
    ).bind(packId).first();
    return ok(c, { pack: created ? toPackResponse(created) : null }, 201);
  });
  app2.patch("/:packId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const packId = c.req.param("packId");
    const existing = await c.env.DB.prepare(
      `SELECT id, creator_user_id, title, moderation_status,
        config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
    ).bind(packId).first();
    if (!existing) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (existing.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }
    const body = await safeJson(c.req.raw);
    if (!body) {
      return fail(c, "INVALID_BODY", "Invalid request body", 422);
    }
    const title = body.title !== void 0 ? body.title.trim() : existing.title;
    if (!title) {
      return fail(c, "INVALID_TITLE", "title cannot be empty", 422);
    }
    const existingConfig = parsePackConfig(existing.config_json);
    const nextConfig = {
      description: body.description !== void 0 ? body.description?.trim() ?? null : existingConfig.description,
      zipAssetId: body.zipAssetId !== void 0 ? body.zipAssetId?.trim() ?? null : existingConfig.zipAssetId,
      thumbnailAssetId: body.thumbnailAssetId !== void 0 ? body.thumbnailAssetId?.trim() ?? null : existingConfig.thumbnailAssetId
    };
    await c.env.DB.prepare(
      `UPDATE packs SET
          title = ?,
          config_json = ?,
          updated_at = CURRENT_TIMESTAMP
         WHERE id = ?`
    ).bind(title, stringifyPackConfig(nextConfig), packId).run();
    const updated = await c.env.DB.prepare(
      `SELECT id, creator_user_id, title, moderation_status,
          config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
    ).bind(packId).first();
    return ok(c, { pack: updated ? toPackResponse(updated) : null });
  });
  app2.post("/:packId/items", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const packId = c.req.param("packId");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }
    const body = await safeJson(c.req.raw);
    const itemIds = body?.itemIds ?? [];
    if (!Array.isArray(itemIds)) {
      return fail(c, "INVALID_ITEM_IDS", "itemIds must be an array", 422);
    }
    const normalized = itemIds.map((itemId) => itemId.trim()).filter((itemId) => itemId.length > 0);
    if (normalized.length !== new Set(normalized).size) {
      return fail(c, "DUPLICATE_ITEM_IDS", "itemIds must be unique", 422);
    }
    if (normalized.length > 0) {
      const placeholders = normalized.map(() => "?").join(",");
      const owned = await c.env.DB.prepare(`SELECT id FROM items WHERE creator_user_id = ? AND id IN (${placeholders})`).bind(auth.userId, ...normalized).all();
      if (owned.results.length !== normalized.length) {
        return fail(c, "INVALID_ITEMS", "All items must exist and belong to the current user", 422);
      }
    }
    await c.env.DB.prepare("DELETE FROM pack_items WHERE pack_id = ?").bind(packId).run();
    for (let index = 0; index < normalized.length; index++) {
      await c.env.DB.prepare("INSERT INTO pack_items (pack_id, item_id, sort_order) VALUES (?, ?, ?)").bind(packId, normalized[index], index).run();
    }
    return ok(c, { packId, itemIds: normalized });
  });
  app2.post("/:packId/submit", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const packId = c.req.param("packId");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }
    await c.env.DB.prepare("UPDATE packs SET moderation_status = 'pending_review', updated_at = CURRENT_TIMESTAMP WHERE id = ?").bind(packId).run();
    return ok(c, { packId, moderationStatus: "pending_review" });
  });
  app2.post("/:packId/publish", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const packId = c.req.param("packId");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }
    const packItems = await c.env.DB.prepare("SELECT COUNT(*) AS total FROM pack_items WHERE pack_id = ?").bind(packId).first();
    if (!packItems || Number(packItems.total) <= 0) {
      return fail(c, "EMPTY_PACK", "Cannot publish an empty pack", 422);
    }
    await c.env.DB.prepare(
      "UPDATE packs SET moderation_status = 'approved', published_at = COALESCE(published_at, CURRENT_TIMESTAMP), updated_at = CURRENT_TIMESTAMP WHERE id = ?"
    ).bind(packId).run();
    return ok(c, { packId, moderationStatus: "approved" });
  });
  app2.get("/:packId", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");
    const pack = await c.env.DB.prepare(
      `SELECT id, creator_user_id, title, moderation_status,
        config_json, published_at, created_at, updated_at
         FROM packs WHERE id = ?`
    ).bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const packConfig = parsePackConfig(pack.config_json);
    const packItems = await c.env.DB.prepare(
      `SELECT pi.item_id, pi.sort_order, i.title, i.type
         FROM pack_items pi
         JOIN items i ON i.id = pi.item_id
         WHERE pi.pack_id = ?
         ORDER BY pi.sort_order ASC`
    ).bind(packId).all();
    return ok(c, {
      pack: {
        ...toPackResponse(pack),
        thumbnailUrl: packConfig.thumbnailAssetId ? `/v1/packs/${packId}/thumbnail` : null
      },
      items: packItems.results.map((entry) => ({
        itemId: entry.item_id,
        sortOrder: entry.sort_order,
        title: entry.title,
        type: entry.type
      }))
    });
  });
  app2.get("/:packId/thumbnail", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id, moderation_status, config_json FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const config = parsePackConfig(pack.config_json);
    if (!config.thumbnailAssetId) {
      return fail(c, "MISSING_THUMBNAIL", "Pack thumbnail is not available", 404);
    }
    const asset = await c.env.DB.prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?").bind(config.thumbnailAssetId).first();
    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Pack thumbnail asset not found", 404);
    }
    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Pack thumbnail object not found", 404);
    }
    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/octet-stream";
    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType
      }
    });
  });
  app2.get("/:packId/download", optionalAuth, async (c) => {
    const packId = c.req.param("packId");
    const auth = c.get("auth");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id, title, moderation_status, config_json FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const isOwner = auth?.userId === pack.creator_user_id;
    const isPublic = pack.moderation_status === "approved";
    if (!isOwner && !isPublic) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    const config = parsePackConfig(pack.config_json);
    if (!config.zipAssetId) {
      return fail(c, "MISSING_ARCHIVE", "Pack archive is not available", 409);
    }
    const asset = await c.env.DB.prepare("SELECT r2_key, mime_type FROM assets WHERE id = ?").bind(config.zipAssetId).first();
    if (!asset) {
      return fail(c, "ASSET_NOT_FOUND", "Pack archive asset not found", 404);
    }
    const object = await c.env.ASSETS.get(asset.r2_key);
    if (!object || !object.body) {
      return fail(c, "OBJECT_NOT_FOUND", "Pack archive object not found", 404);
    }
    await c.env.DB.prepare("INSERT INTO downloads (id, user_id, item_id, pack_id, created_at) VALUES (?, ?, NULL, ?, CURRENT_TIMESTAMP)").bind(randomId("dwl"), auth?.userId ?? null, packId).run();
    const contentType = object.httpMetadata?.contentType ?? asset.mime_type ?? "application/zip";
    const fileName = downloadFileName2(pack.title, "zip");
    return new Response(object.body, {
      status: 200,
      headers: {
        "content-type": contentType,
        "content-disposition": `attachment; filename="${fileName}"`
      }
    });
  });
  app2.delete("/:packId", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const packId = c.req.param("packId");
    const pack = await c.env.DB.prepare("SELECT id, creator_user_id FROM packs WHERE id = ?").bind(packId).first();
    if (!pack) {
      return fail(c, "NOT_FOUND", "Pack not found", 404);
    }
    if (pack.creator_user_id !== auth.userId) {
      return fail(c, "FORBIDDEN", "You do not own this pack", 403);
    }
    await c.env.DB.prepare("DELETE FROM pack_items WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM featured_row_items WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM downloads WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM reports WHERE pack_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM moderation_actions WHERE target_type = 'pack' AND target_id = ?").bind(packId).run();
    await c.env.DB.prepare("DELETE FROM packs WHERE id = ?").bind(packId).run();
    return ok(c, { packId, deleted: true });
  });
  return app2;
}
__name(packRoutes, "packRoutes");

// src/routes/uploads.ts
var allowedKinds = /* @__PURE__ */ new Set(["item_payload", "item_manifest", "pack_zip", "thumbnail", "preview_audio"]);
var decoder = new TextDecoder();
function hasSignature(bytes, signature, offset = 0) {
  if (offset < 0 || bytes.length < offset + signature.length) {
    return false;
  }
  for (let index = 0; index < signature.length; index++) {
    if (bytes[offset + index] !== signature[index]) {
      return false;
    }
  }
  return true;
}
__name(hasSignature, "hasSignature");
function getExtension(path) {
  const fileName = path.split("/").pop() ?? path;
  const dotIndex = fileName.lastIndexOf(".");
  if (dotIndex <= 0 || dotIndex === fileName.length - 1) {
    return "";
  }
  return fileName.slice(dotIndex).toLowerCase();
}
__name(getExtension, "getExtension");
function isZipBuffer(bytes) {
  if (bytes.length < 4) {
    return false;
  }
  return bytes[0] === 80 && bytes[1] === 75 && (bytes[2] === 3 || bytes[2] === 5 || bytes[2] === 7) && (bytes[3] === 4 || bytes[3] === 6 || bytes[3] === 8);
}
__name(isZipBuffer, "isZipBuffer");
function parseZipEntryNames(body) {
  const view = new DataView(body);
  const bytes = new Uint8Array(body);
  const minimumEocdLength = 22;
  let eocdOffset = -1;
  const searchStart = Math.max(0, view.byteLength - 65557);
  for (let offset = view.byteLength - minimumEocdLength; offset >= searchStart; offset--) {
    if (view.getUint32(offset, true) === 101010256) {
      eocdOffset = offset;
      break;
    }
  }
  if (eocdOffset < 0) {
    throw new Error("ZIP end-of-central-directory record missing");
  }
  const totalEntries = view.getUint16(eocdOffset + 10, true);
  const centralDirectoryOffset = view.getUint32(eocdOffset + 16, true);
  if (centralDirectoryOffset >= view.byteLength) {
    throw new Error("ZIP central directory offset is invalid");
  }
  let cursor = centralDirectoryOffset;
  const entries = [];
  for (let index = 0; index < totalEntries; index++) {
    if (cursor + 46 > view.byteLength || view.getUint32(cursor, true) !== 33639248) {
      throw new Error("ZIP central directory entry is invalid");
    }
    const fileNameLength = view.getUint16(cursor + 28, true);
    const extraLength = view.getUint16(cursor + 30, true);
    const commentLength = view.getUint16(cursor + 32, true);
    const nameStart = cursor + 46;
    const nameEnd = nameStart + fileNameLength;
    if (nameEnd > view.byteLength) {
      throw new Error("ZIP file name range is invalid");
    }
    const entryName = decoder.decode(bytes.subarray(nameStart, nameEnd));
    entries.push(entryName);
    cursor = nameEnd + extraLength + commentLength;
  }
  return entries;
}
__name(parseZipEntryNames, "parseZipEntryNames");
function validatePresetArchive(body) {
  const bytes = new Uint8Array(body);
  if (!isZipBuffer(bytes)) {
    return { ok: false, reason: "Preset payload must be a ZIP archive" };
  }
  let entries;
  try {
    entries = parseZipEntryNames(body);
  } catch (error) {
    return { ok: false, reason: error instanceof Error ? error.message : "Invalid ZIP archive" };
  }
  const fileEntries = entries.filter((entry) => !entry.endsWith("/"));
  if (fileEntries.length === 0) {
    return { ok: false, reason: "Preset archive is empty" };
  }
  const allowedExtensions = /* @__PURE__ */ new Set([".json", ".wav", ".nam"]);
  let hasJson = false;
  let hasResource = false;
  for (const entry of fileEntries) {
    const extension = getExtension(entry);
    if (!allowedExtensions.has(extension)) {
      return { ok: false, reason: `Unsupported file extension in preset archive: ${extension || "(none)"}` };
    }
    if (extension === ".json") {
      hasJson = true;
    }
    if (extension === ".wav" || extension === ".nam") {
      hasResource = true;
    }
  }
  if (!hasJson) {
    return { ok: false, reason: "Preset archive must include at least one JSON file" };
  }
  if (!hasResource) {
    return { ok: false, reason: "Preset archive must include at least one .wav or .nam file" };
  }
  return { ok: true };
}
__name(validatePresetArchive, "validatePresetArchive");
function validatePackArchive(body) {
  const bytes = new Uint8Array(body);
  if (!isZipBuffer(bytes)) {
    return { ok: false, reason: "Pack upload must be a ZIP archive" };
  }
  try {
    const entries = parseZipEntryNames(body);
    const fileEntries = entries.filter((entry) => !entry.endsWith("/"));
    if (fileEntries.length === 0) {
      return { ok: false, reason: "Pack archive is empty" };
    }
  } catch (error) {
    return { ok: false, reason: error instanceof Error ? error.message : "Invalid ZIP archive" };
  }
  return { ok: true };
}
__name(validatePackArchive, "validatePackArchive");
function validateJsonDocument(body) {
  try {
    const text = decoder.decode(new Uint8Array(body));
    JSON.parse(text);
  } catch {
    return { ok: false, reason: "Manifest must be valid JSON" };
  }
  return { ok: true };
}
__name(validateJsonDocument, "validateJsonDocument");
function validateThumbnail(body) {
  const bytes = new Uint8Array(body);
  const isPng = hasSignature(bytes, [137, 80, 78, 71, 13, 10, 26, 10]);
  const isJpeg = hasSignature(bytes, [255, 216, 255]);
  const isWebp = hasSignature(bytes, [82, 73, 70, 70]) && hasSignature(bytes, [87, 69, 66, 80], 8);
  if (!isPng && !isJpeg && !isWebp) {
    return { ok: false, reason: "Thumbnail must be PNG, JPEG, or WEBP" };
  }
  return { ok: true };
}
__name(validateThumbnail, "validateThumbnail");
function validatePreviewAudio(body) {
  const bytes = new Uint8Array(body);
  const isWav = hasSignature(bytes, [82, 73, 70, 70]) && hasSignature(bytes, [87, 65, 86, 69], 8);
  const isMp3 = hasSignature(bytes, [73, 68, 51]) || bytes.length > 1 && bytes[0] === 255 && (bytes[1] & 224) === 224;
  const isOgg = hasSignature(bytes, [79, 103, 103, 83]);
  if (!isWav && !isMp3 && !isOgg) {
    return { ok: false, reason: "Preview audio must be WAV, MP3, or OGG" };
  }
  return { ok: true };
}
__name(validatePreviewAudio, "validatePreviewAudio");
function validateUploadedContent(kind, body) {
  if (kind === "item_payload") {
    return validatePresetArchive(body);
  }
  if (kind === "pack_zip") {
    return validatePackArchive(body);
  }
  if (kind === "item_manifest") {
    return validateJsonDocument(body);
  }
  if (kind === "thumbnail") {
    return validateThumbnail(body);
  }
  if (kind === "preview_audio") {
    return validatePreviewAudio(body);
  }
  return { ok: false, reason: "Unknown upload kind" };
}
__name(validateUploadedContent, "validateUploadedContent");
function uploadRoutes() {
  const app2 = new Hono2();
  app2.post("/init", requireAuth, async (c) => {
    const body = await safeJson(c.req.raw);
    const kind = body?.kind;
    const mimeType = body?.mimeType?.trim();
    const byteSize = body?.byteSize ?? 0;
    if (!kind || !allowedKinds.has(kind)) {
      return fail(c, "INVALID_KIND", "Invalid upload kind", 422);
    }
    if (!mimeType || byteSize <= 0) {
      return fail(c, "INVALID_METADATA", "mimeType and byteSize are required", 422);
    }
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const uploadId = randomId("upl");
    const r2Key = `users/${auth.userId}/drafts/${uploadId}/payload.bin`;
    await c.env.DB.prepare("INSERT INTO assets (id, owner_user_id, r2_key, kind, mime_type, byte_size, uploaded_at) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)").bind(uploadId, auth.userId, r2Key, kind, mimeType, byteSize).run();
    return ok(c, {
      uploadId,
      kind,
      mimeType,
      byteSize,
      uploadUrl: `/v1/uploads/${uploadId}`,
      r2Key
    });
  });
  app2.put("/:uploadId", requireAuth, async (c) => {
    const uploadId = c.req.param("uploadId");
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }
    const mimeType = c.req.header("content-type") ?? "application/octet-stream";
    const body = await c.req.arrayBuffer();
    if (body.byteLength === 0) {
      return fail(c, "EMPTY_BODY", "Upload payload is empty", 422);
    }
    const asset = await c.env.DB.prepare("SELECT id, owner_user_id, kind, byte_size FROM assets WHERE id = ?").bind(uploadId).first();
    if (!asset || asset.owner_user_id !== auth.userId) {
      return fail(c, "NOT_FOUND", "Upload session not found", 404);
    }
    if (Number(asset.byte_size) !== body.byteLength) {
      return fail(c, "INVALID_SIZE", "Uploaded byte size does not match initialized byteSize", 422);
    }
    const validation = validateUploadedContent(asset.kind, body);
    if (!validation.ok) {
      return fail(c, "INVALID_UPLOAD_CONTENT", validation.reason, 422);
    }
    const r2Key = `users/${auth.userId}/drafts/${uploadId}/payload.bin`;
    await c.env.ASSETS.put(r2Key, body, {
      httpMetadata: {
        contentType: mimeType
      }
    });
    await c.env.DB.prepare("UPDATE assets SET r2_key = ?, mime_type = ?, byte_size = ?, uploaded_at = CURRENT_TIMESTAMP WHERE id = ?").bind(r2Key, mimeType, body.byteLength, uploadId).run();
    return ok(c, {
      uploadId,
      r2Key,
      byteSize: body.byteLength
    });
  });
  app2.post("/complete", requireAuth, async (c) => {
    const body = await safeJson(c.req.raw);
    const uploadId = body?.uploadId?.trim();
    if (!uploadId) {
      return fail(c, "INVALID_UPLOAD_ID", "uploadId is required", 422);
    }
    const asset = await c.env.DB.prepare("SELECT id, owner_user_id, r2_key, byte_size FROM assets WHERE id = ?").bind(uploadId).first();
    const auth = c.get("auth");
    if (!asset || !auth || asset.owner_user_id !== auth.userId) {
      return fail(c, "NOT_FOUND", "Upload not found", 404);
    }
    const object = await c.env.ASSETS.head(asset.r2_key);
    if (!object) {
      return fail(c, "MISSING_OBJECT", "Uploaded object not found in storage", 409);
    }
    return ok(c, {
      assetId: asset.id,
      r2Key: asset.r2_key,
      byteSize: asset.byte_size
    });
  });
  return app2;
}
__name(uploadRoutes, "uploadRoutes");

// src/index.ts
var app = new Hono2();
app.use(
  "*",
  cors({
    origin: /* @__PURE__ */ __name((origin) => origin || "*", "origin"),
    credentials: true,
    allowMethods: ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
    exposeHeaders: ["content-disposition", "content-length", "content-type"],
    maxAge: 86400
  })
);
app.route("/", healthRoutes());
app.route("/v1/auth", authRoutes());
app.route("/v1", discoveryRoutes());
app.route("/v1/items", itemRoutes());
app.route("/v1/packs", packRoutes());
app.route("/v1/uploads", uploadRoutes());
app.notFound((c) => fail(c, "NOT_FOUND", "Route not found", 404));
app.onError((error, c) => {
  return fail(c, "INTERNAL_ERROR", error.message || "Unexpected error", 500);
});
var src_default = app;

// node_modules/wrangler/templates/middleware/middleware-ensure-req-body-drained.ts
var drainBody = /* @__PURE__ */ __name(async (request, env, _ctx, middlewareCtx) => {
  try {
    return await middlewareCtx.next(request, env);
  } finally {
    try {
      if (request.body !== null && !request.bodyUsed) {
        const reader = request.body.getReader();
        while (!(await reader.read()).done) {
        }
      }
    } catch (e) {
      console.error("Failed to drain the unused request body.", e);
    }
  }
}, "drainBody");
var middleware_ensure_req_body_drained_default = drainBody;

// node_modules/wrangler/templates/middleware/middleware-miniflare3-json-error.ts
function reduceError(e) {
  return {
    name: e?.name,
    message: e?.message ?? String(e),
    stack: e?.stack,
    cause: e?.cause === void 0 ? void 0 : reduceError(e.cause)
  };
}
__name(reduceError, "reduceError");
var jsonError = /* @__PURE__ */ __name(async (request, env, _ctx, middlewareCtx) => {
  try {
    return await middlewareCtx.next(request, env);
  } catch (e) {
    const error = reduceError(e);
    return Response.json(error, {
      status: 500,
      headers: { "MF-Experimental-Error-Stack": "true" }
    });
  }
}, "jsonError");
var middleware_miniflare3_json_error_default = jsonError;

// .wrangler/tmp/bundle-OF1x3O/middleware-insertion-facade.js
var __INTERNAL_WRANGLER_MIDDLEWARE__ = [
  middleware_ensure_req_body_drained_default,
  middleware_miniflare3_json_error_default
];
var middleware_insertion_facade_default = src_default;

// node_modules/wrangler/templates/middleware/common.ts
var __facade_middleware__ = [];
function __facade_register__(...args) {
  __facade_middleware__.push(...args.flat());
}
__name(__facade_register__, "__facade_register__");
function __facade_invokeChain__(request, env, ctx, dispatch, middlewareChain) {
  const [head, ...tail] = middlewareChain;
  const middlewareCtx = {
    dispatch,
    next(newRequest, newEnv) {
      return __facade_invokeChain__(newRequest, newEnv, ctx, dispatch, tail);
    }
  };
  return head(request, env, ctx, middlewareCtx);
}
__name(__facade_invokeChain__, "__facade_invokeChain__");
function __facade_invoke__(request, env, ctx, dispatch, finalMiddleware) {
  return __facade_invokeChain__(request, env, ctx, dispatch, [
    ...__facade_middleware__,
    finalMiddleware
  ]);
}
__name(__facade_invoke__, "__facade_invoke__");

// .wrangler/tmp/bundle-OF1x3O/middleware-loader.entry.ts
var __Facade_ScheduledController__ = class ___Facade_ScheduledController__ {
  constructor(scheduledTime, cron, noRetry) {
    this.scheduledTime = scheduledTime;
    this.cron = cron;
    this.#noRetry = noRetry;
  }
  static {
    __name(this, "__Facade_ScheduledController__");
  }
  #noRetry;
  noRetry() {
    if (!(this instanceof ___Facade_ScheduledController__)) {
      throw new TypeError("Illegal invocation");
    }
    this.#noRetry();
  }
};
function wrapExportedHandler(worker) {
  if (__INTERNAL_WRANGLER_MIDDLEWARE__ === void 0 || __INTERNAL_WRANGLER_MIDDLEWARE__.length === 0) {
    return worker;
  }
  for (const middleware of __INTERNAL_WRANGLER_MIDDLEWARE__) {
    __facade_register__(middleware);
  }
  const fetchDispatcher = /* @__PURE__ */ __name(function(request, env, ctx) {
    if (worker.fetch === void 0) {
      throw new Error("Handler does not export a fetch() function.");
    }
    return worker.fetch(request, env, ctx);
  }, "fetchDispatcher");
  return {
    ...worker,
    fetch(request, env, ctx) {
      const dispatcher = /* @__PURE__ */ __name(function(type, init) {
        if (type === "scheduled" && worker.scheduled !== void 0) {
          const controller = new __Facade_ScheduledController__(
            Date.now(),
            init.cron ?? "",
            () => {
            }
          );
          return worker.scheduled(controller, env, ctx);
        }
      }, "dispatcher");
      return __facade_invoke__(request, env, ctx, dispatcher, fetchDispatcher);
    }
  };
}
__name(wrapExportedHandler, "wrapExportedHandler");
function wrapWorkerEntrypoint(klass) {
  if (__INTERNAL_WRANGLER_MIDDLEWARE__ === void 0 || __INTERNAL_WRANGLER_MIDDLEWARE__.length === 0) {
    return klass;
  }
  for (const middleware of __INTERNAL_WRANGLER_MIDDLEWARE__) {
    __facade_register__(middleware);
  }
  return class extends klass {
    #fetchDispatcher = /* @__PURE__ */ __name((request, env, ctx) => {
      this.env = env;
      this.ctx = ctx;
      if (super.fetch === void 0) {
        throw new Error("Entrypoint class does not define a fetch() function.");
      }
      return super.fetch(request);
    }, "#fetchDispatcher");
    #dispatcher = /* @__PURE__ */ __name((type, init) => {
      if (type === "scheduled" && super.scheduled !== void 0) {
        const controller = new __Facade_ScheduledController__(
          Date.now(),
          init.cron ?? "",
          () => {
          }
        );
        return super.scheduled(controller);
      }
    }, "#dispatcher");
    fetch(request) {
      return __facade_invoke__(
        request,
        this.env,
        this.ctx,
        this.#dispatcher,
        this.#fetchDispatcher
      );
    }
  };
}
__name(wrapWorkerEntrypoint, "wrapWorkerEntrypoint");
var WRAPPED_ENTRY;
if (typeof middleware_insertion_facade_default === "object") {
  WRAPPED_ENTRY = wrapExportedHandler(middleware_insertion_facade_default);
} else if (typeof middleware_insertion_facade_default === "function") {
  WRAPPED_ENTRY = wrapWorkerEntrypoint(middleware_insertion_facade_default);
}
var middleware_loader_entry_default = WRAPPED_ENTRY;
export {
  __INTERNAL_WRANGLER_MIDDLEWARE__,
  middleware_loader_entry_default as default
};
//# sourceMappingURL=index.js.map
