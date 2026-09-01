/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

// ============================================================================
// VENDORED FROM JUCE — DO NOT EDIT CASUALLY.
//
// Source: juce/JUCE/modules/juce_gui_extra/native/java/app/com/rmsl/juce/
//         JuceWebViewClasses.java   (JUCE 8.0.14)
//
// JUCE resolves this class through the app's class loader before falling back
// to the copy it embeds as bytecode (see JNIClassBase::initialise), so a copy
// living here wins. Copying JUCE's java/ sources into the app project is the
// mechanism JUCE itself expects — it is what the Projucer does — but it means
// this file is a fork and has to be re-synced when JUCE is updated.
//
// The build guards against silent drift: android/app/build.gradle.kts records
// the SHA-256 of the upstream file and the checkVendoredJuceWebView task fails
// if upstream changes, so the patch below gets re-applied deliberately.
//
// THE DELTA vs upstream is two changes to JuceWebView, both marked with
// "Soundshed patch" comments, both fixing an Android WebView default that
// leaves the UI dead on arrival:
//
//   1. settings.setDomStorageEnabled (true)
//      Without it window.localStorage is null and UI bootstrap throws.
//
//   2. an onSizeChanged override that pins LayoutParams and measures
//      JUCE leaves the view on WRAP_CONTENT and never measures it, which makes
//      every percentage and vh height in the page resolve to 0.
//
//   3. two calls to com.soundshed.guitar.SafeAreaInsets.apply()
//      The activity cannot reach this WebView — JUCE gives its peer view a
//      window of its own through WindowManager — so the safe-area insets are
//      published from here instead. The logic lives in SafeAreaInsets; these
//      stay one-liners.
//
// Keep it to that — anything else belongs in our own code, not in a copy of a
// framework file.
// ============================================================================

package com.rmsl.juce;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.http.SslError;
import android.os.Build;
import android.os.Message;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebResourceResponse;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.SslErrorHandler;
import android.webkit.WebChromeClient;

import java.lang.annotation.Native;
import java.util.ArrayList;

//==============================================================================
public class JuceWebViewClasses
{
    static public class NativeInterface
    {
        private long host;
        private final Object hostLock = new Object ();

        //==============================================================================
        public NativeInterface (long hostToUse)
        {
            host = hostToUse;
        }

        public void hostDeleted ()
        {
            synchronized (hostLock)
            {
                host = 0;
            }
        }

        //==============================================================================
        public void onPageStarted (WebView view, String url)
        {
            if (host == 0)
                return;

            handleResourceRequest (host, view, url);
        }

        public WebResourceResponse shouldInterceptRequest (WebView view, WebResourceRequest request)
        {
            synchronized (hostLock)
            {
                if (host != 0)
                {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP)
                        return handleResourceRequest (host, view, request.getUrl().toString());
                }
            }

            return null;
        }

        public void onPageFinished (WebView view, String url)
        {
            // ---- Soundshed patch ----------------------------------------
            // Safe-area insets are delivered as inline styles on the root
            // element, which a navigation discards — and JUCE always navigates
            // once after creating the view. This is the only reliable "the page
            // is ready" hook we have. The logic lives in our own class; keep
            // this to the single call.
            com.soundshed.guitar.SafeAreaInsets.apply (view);
            // -------------------------------------------------------------

            if (host == 0)
                return;

            webViewPageLoadFinished (host, view, url);
        }

        public void onReceivedSslError (WebView view, SslErrorHandler handler, SslError error)
        {
            if (host == 0)
                return;

            webViewReceivedSslError (host, view, handler, error);
        }

        public void onReceivedHttpError (WebView view, WebResourceRequest request, WebResourceResponse errorResponse)
        {
            if (host == 0)
                return;

            webViewReceivedHttpError (host, view, request, errorResponse);
        }

        public void onCloseWindow (WebView window)
        {
            if (host == 0)
                return;

            webViewCloseWindowRequest (host, window);
        }

        public boolean onCreateWindow (WebView view, boolean isDialog,
                                       boolean isUserGesture, Message resultMsg)
        {
            if (host == 0)
                return false;

            webViewCreateWindowRequest (host, view);
            return false;
        }

        public String postMessageHandler (String message)
        {
            synchronized (hostLock)
            {
                if (host != 0)
                {
                    String result = postMessage (host, message);

                    if (result == null)
                        return "";

                    return result;
                }
            }

            return "";
        }

        public void handleJavascriptEvaluationResult (long evalId, String result)
        {
            if (host == 0)
                return;

            evaluationResultHandler (host, evalId, result);
        }

        public boolean shouldOverrideUrlLoading (String url)
        {
            if (host == 0)
                return false;

            return ! pageAboutToLoad (host, url);
        }

        //==============================================================================
        private native WebResourceResponse handleResourceRequest (long host, WebView view, String url);
        private native void webViewPageLoadFinished (long host, WebView view, String url);
        private native void webViewReceivedSslError (long host, WebView view, SslErrorHandler handler, SslError error);
        private native void webViewReceivedHttpError (long host, WebView view, WebResourceRequest request, WebResourceResponse errorResponse);

        private native void webViewCloseWindowRequest (long host, WebView view);
        private native void webViewCreateWindowRequest (long host, WebView view);

        private native void evaluationResultHandler (long host, long evalId, String result);
        private native String postMessage (long host, String message);

        private native boolean pageAboutToLoad (long host, String url);
    }

    static public class Client extends WebViewClient
    {
        private NativeInterface nativeInterface;

        //==============================================================================
        public Client (NativeInterface nativeInterfaceIn)
        {
            nativeInterface = nativeInterfaceIn;
        }

        //==============================================================================
        @Override
        public void onPageFinished (WebView view, String url)
        {
            nativeInterface.onPageFinished (view, url);
        }

        @Override
        public void onReceivedSslError (WebView view, SslErrorHandler handler, SslError error)
        {
            nativeInterface.onReceivedSslError (view, handler, error);
        }

        @Override
        public void onReceivedHttpError (WebView view, WebResourceRequest request, WebResourceResponse errorResponse)
        {
            nativeInterface.onReceivedHttpError (view, request, errorResponse);
        }

        @Override
        public void onPageStarted (WebView view, String url, Bitmap favicon)
        {
            nativeInterface.onPageStarted (view, url);
        }

        @Override
        public WebResourceResponse shouldInterceptRequest (WebView view, WebResourceRequest request)
        {
            return nativeInterface.shouldInterceptRequest (view, request);
        }

        @Override
        public boolean shouldOverrideUrlLoading (WebView view, WebResourceRequest request)
        {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP)
                return nativeInterface.shouldOverrideUrlLoading (request.getUrl().toString());

            return false;
        }

        @Override
        public boolean shouldOverrideUrlLoading (WebView view, String url)
        {
            return nativeInterface.shouldOverrideUrlLoading (url);
        }
    }

    static public class ChromeClient extends WebChromeClient
    {
        private NativeInterface nativeInterface;

        //==============================================================================
        public ChromeClient (NativeInterface nativeInterfaceIn)
        {
            nativeInterface = nativeInterfaceIn;
        }

        //==============================================================================
        @Override
        public void onCloseWindow (WebView window)
        {
            nativeInterface.onCloseWindow (window);
        }

        @Override
        public boolean onCreateWindow (WebView view, boolean isDialog,
                                       boolean isUserGesture, Message resultMsg)
        {
            return nativeInterface.onCreateWindow (view, isDialog, isUserGesture, resultMsg);
        }
    }

    static public class WebAppInterface
    {
        private NativeInterface nativeInterface;
        String userScripts;

        //==============================================================================
        public WebAppInterface (NativeInterface nativeInterfaceIn, String userScriptsIn)
        {
            nativeInterface = nativeInterfaceIn;
            userScripts = userScriptsIn;
        }

        //==============================================================================
        @JavascriptInterface
        public String postMessage (String message)
        {
            return nativeInterface.postMessageHandler (message);
        }

        @JavascriptInterface
        public String getAndroidUserScripts()
        {
            return userScripts;
        }
    }

    static public class JuceWebView extends WebView
    {
        private NativeInterface nativeInterface;
        private long evaluationId = 0;

        public JuceWebView (Context context, long host, String userAgent, String initScripts)
        {
            super (context);

            nativeInterface = new NativeInterface (host);

            WebSettings settings = getSettings();
            settings.setJavaScriptEnabled (true);
            // ---- Soundshed patch ----------------------------------------
            // Android WebView leaves DOM storage disabled by default, which
            // makes window.localStorage null rather than merely empty. The UI
            // reads it during startup, so upstream's defaults produce
            // "TypeError: Cannot read properties of null (reading 'getItem')"
            // and a dead app.
            settings.setDomStorageEnabled (true);
            // -------------------------------------------------------------
            settings.setBuiltInZoomControls (true);
            settings.setDisplayZoomControls (false);
            settings.setSupportMultipleWindows (true);
            settings.setUserAgentString (userAgent);

            setWebChromeClient (new ChromeClient (nativeInterface));

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT)
            {
                setWebContentsDebuggingEnabled (true);
            }

            setWebViewClient (new Client (nativeInterface));
            addJavascriptInterface (new WebAppInterface (nativeInterface, initScripts), "__JUCE__");
        }

        // ---- Soundshed patch --------------------------------------------
        // Give the view a definite size of its own.
        //
        // JUCE adds this WebView with addView(view) and no LayoutParams, so it
        // inherits ViewGroup's WRAP_CONTENT default, and then positions it by
        // calling layout() directly (juce_AndroidViewComponent.cpp). JUCE's
        // ComponentPeerView.onLayout is empty, so nothing ever measures it
        // either. A WRAP_CONTENT WebView is the case Android documents as
        // unsupported for percentage heights, and the symptom here is a very
        // misleading one: the page loads and renders, JS reports a correct
        // window.innerHeight, but every percentage and vh/dvh height resolves
        // to 0. "html, body { height: 100% }" then collapses the entire UI to
        // nothing and the app shows a blank screen with a healthy page in it.
        //
        // Pinning LayoutParams to our bounds and measuring against them makes
        // the layout viewport definite. ViewGroup.layout() is final so it
        // cannot be overridden; onSizeChanged is the first hook that runs with
        // the new bounds.
        @Override
        protected void onSizeChanged (int w, int h, final int oldw, final int oldh)
        {
            super.onSizeChanged (w, h, oldw, oldh);

            if (w <= 0 || h <= 0)
                return;

            final int width = w, height = h;
            final int left = getLeft(), top = getTop();

            // Posted rather than run inline: onSizeChanged arrives from inside
            // layout(), and a measure started there is discarded.
            post (new Runnable()
            {
                @Override
                public void run()
                {
                    if (getWidth() != width || getHeight() != height)
                        return; // a newer size has already superseded this one

                    // JUCE adds this view with addView(view) and no LayoutParams,
                    // so it inherits ViewGroup's WRAP_CONTENT default. That is the
                    // state Android's WebView documents as unsupported: percentage
                    // and viewport heights stop resolving. Pin them to our bounds.
                    android.view.ViewGroup.LayoutParams lp = getLayoutParams();

                    if (lp == null)
                        lp = new android.view.ViewGroup.LayoutParams (width, height);

                    if (lp.width != width || lp.height != height)
                    {
                        lp.width = width;
                        lp.height = height;
                        setLayoutParams (lp);
                    }

                    measure (MeasureSpec.makeMeasureSpec (width,  MeasureSpec.EXACTLY),
                             MeasureSpec.makeMeasureSpec (height, MeasureSpec.EXACTLY));

                    // layout() is final so it cannot be overridden, but it can
                    // be called: re-applying the same bounds is what makes the
                    // measured size take effect. The size is unchanged, so this
                    // does not re-enter onSizeChanged.
                    //
                    // Whether all three steps are needed is UNVERIFIED. The
                    // LayoutParams line was added last, on top of the measure and
                    // layout calls, and dropping those two has not been tested
                    // under conditions worth trusting — the one attempt measured
                    // a bogus viewport because the app had been launched over adb
                    // onto a locked screen, which gives the window portrait
                    // dimensions regardless. If you want to trim this, retest with
                    // the device unlocked and the app launched normally.
                    layout (left, top, left + width, top + height);

                    // Rotation changes which edges the system bars occupy.
                    com.soundshed.guitar.SafeAreaInsets.apply (JuceWebView.this);
                }
            });
        }
        // -----------------------------------------------------------------

        public void disconnectNative()
        {
            nativeInterface.hostDeleted();
        }

        public long evaluateJavascript (String script)
        {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT)
            {
                final long currentEvaluationId = evaluationId++;
                final NativeInterface accessibleNativeInterface = nativeInterface;

                super.evaluateJavascript (script, new ValueCallback<String>()
                                                  {
                                                      @Override
                                                      public void onReceiveValue(String result)
                                                      {
                                                          accessibleNativeInterface.handleJavascriptEvaluationResult (currentEvaluationId,
                                                                                                                      result);
                                                      }
                                                  });

                return currentEvaluationId;
            }

            return -1;
        }
    }
}
