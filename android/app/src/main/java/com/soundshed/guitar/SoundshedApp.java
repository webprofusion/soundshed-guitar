package com.soundshed.guitar;

import android.content.res.AssetManager;
import android.util.Log;

import com.rmsl.juce.JuceApp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Application entry point.
 *
 * <p>The web UI is packaged into the APK as assets under "ui/", but the editor
 * and several controller services resolve their resources as ordinary files on
 * disk ("&lt;root&gt;/ui/index.html", bundled presets, metronome clicks, effect
 * layout images). Rather than teach every one of those paths about
 * AssetManager, the asset tree is unpacked once into the app's private data
 * directory and the existing file-based code runs unchanged.
 *
 * <p>The unpack target is "&lt;dataDir&gt;/resources", which is exactly what the
 * native side reports from {@code PluginProcessorAdapter::GetBundledAssetsPath()}
 * via JUCE's {@code userApplicationDataDirectory}.
 *
 * <p>Extraction is skipped when the stamp file written by Gradle's
 * {@code stageUiAssets} task already matches the unpacked copy, so it is a
 * first-run (and post-upgrade) cost only.
 */
public class SoundshedApp extends JuceApp
{
    private static final String TAG = "SoundshedGuitar";

    /** Asset subdirectory holding the web UI, and its name once unpacked. */
    private static final String UI_ASSET_DIR = "ui";

    /** Directory under the app data dir that becomes the native resource root. */
    private static final String RESOURCE_DIR_NAME = "resources";

    private static final String STAMP_NAME = "asset-stamp.txt";

    @Override
    public void onCreate()
    {
        // Unpack before super.onCreate(), which loads libjuce_jni.so and starts
        // JUCE — by the time the editor asks for index.html the files must exist.
        try
        {
            unpackUiAssetsIfStale();
        }
        catch (IOException e)
        {
            // A failed unpack is not fatal: the editor falls back to an
            // explanatory page, and the log says why.
            Log.e (TAG, "Failed to unpack UI assets", e);
        }

        super.onCreate();
    }

    /** The directory the native side treats as its resource root. */
    private File resourceRoot()
    {
        return new File (getApplicationInfo().dataDir, RESOURCE_DIR_NAME);
    }

    private void unpackUiAssetsIfStale() throws IOException
    {
        final File root = resourceRoot();
        final File uiDir = new File (root, UI_ASSET_DIR);
        final File installedStamp = new File (uiDir, STAMP_NAME);

        final String packagedStamp = readAssetText (UI_ASSET_DIR + "/" + STAMP_NAME);

        if (packagedStamp != null && installedStamp.isFile())
        {
            final String installed = readFileText (installedStamp);

            if (packagedStamp.equals (installed))
            {
                Log.i (TAG, "UI assets already current at " + uiDir);
                return;
            }
        }

        Log.i (TAG, "Unpacking UI assets to " + uiDir);
        final long startedAt = System.currentTimeMillis();

        deleteRecursively (uiDir);

        if (! uiDir.mkdirs() && ! uiDir.isDirectory())
            throw new IOException ("Could not create " + uiDir);

        copyAssetTree (UI_ASSET_DIR, uiDir);

        Log.i (TAG, "Unpacked UI assets in " + (System.currentTimeMillis() - startedAt) + " ms");
    }

    /** Recursively copies an asset directory into a destination directory. */
    private void copyAssetTree (String assetPath, File destDir) throws IOException
    {
        final AssetManager assets = getAssets();
        final String[] entries = assets.list (assetPath);

        if (entries == null || entries.length == 0)
        {
            // A leaf: AssetManager reports files as empty directories.
            copyAssetFile (assetPath, destDir);
            return;
        }

        for (String entry : entries)
        {
            final String childAsset = assetPath + "/" + entry;
            final String[] grandChildren = assets.list (childAsset);

            if (grandChildren != null && grandChildren.length > 0)
            {
                final File childDir = new File (destDir, entry);

                if (! childDir.mkdirs() && ! childDir.isDirectory())
                    throw new IOException ("Could not create " + childDir);

                copyAssetTree (childAsset, childDir);
            }
            else
            {
                copyAssetFile (childAsset, destDir);
            }
        }
    }

    private void copyAssetFile (String assetPath, File destDir) throws IOException
    {
        final String name = assetPath.substring (assetPath.lastIndexOf ('/') + 1);
        final File dest = new File (destDir, name);

        try (InputStream in = getAssets().open (assetPath);
             OutputStream out = new FileOutputStream (dest))
        {
            final byte[] buffer = new byte[64 * 1024];

            for (int read; (read = in.read (buffer)) != -1;)
                out.write (buffer, 0, read);
        }
        catch (IOException e)
        {
            throw new IOException ("Failed copying asset " + assetPath + " -> " + dest, e);
        }
    }

    private String readAssetText (String assetPath)
    {
        try (InputStream in = getAssets().open (assetPath))
        {
            return new String (readAll (in), StandardCharsets.UTF_8).trim();
        }
        catch (IOException e)
        {
            return null;
        }
    }

    private static String readFileText (File file)
    {
        try (InputStream in = new java.io.FileInputStream (file))
        {
            return new String (readAll (in), StandardCharsets.UTF_8).trim();
        }
        catch (IOException e)
        {
            return null;
        }
    }

    private static byte[] readAll (InputStream in) throws IOException
    {
        final java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        final byte[] buffer = new byte[8192];

        for (int read; (read = in.read (buffer)) != -1;)
            out.write (buffer, 0, read);

        return out.toByteArray();
    }

    private static void deleteRecursively (File file)
    {
        if (file.isDirectory())
        {
            final File[] children = file.listFiles();

            if (children != null)
                for (File child : children)
                    deleteRecursively (child);
        }

        // Best effort: a leftover file only costs disk, and the copy overwrites.
        file.delete();
    }
}
