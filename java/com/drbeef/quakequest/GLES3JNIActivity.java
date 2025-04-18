
package com.drbeef.quakequest;

import static android.system.Os.setenv;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;

import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.view.KeyEvent;

import android.support.v4.app.ActivityCompat;
import android.support.v4.content.ContextCompat;

@SuppressLint("SdCardPath") public class GLES3JNIActivity extends Activity implements SurfaceHolder.Callback
{
	private static String manufacturer = "";

	// Load the gles3jni library right away to make sure JNI_OnLoad() gets called as the very first thing.
	static
	{
		manufacturer = Build.MANUFACTURER.toLowerCase(Locale.ROOT);
		if (manufacturer.contains("oculus")) // rename oculus to meta as this will probably happen in the future anyway
		{
			manufacturer = "meta";
		}

		try
		{
			//Load manufacturer specific loader
			System.loadLibrary("openxr_loader_" + manufacturer);
			setenv("OPENXR_HMD", manufacturer, true);
		} catch (Exception e)
		{}

		System.loadLibrary( "quakequest" );
	}

	private static final String TAG = "QuakeQuest";

	String commandLineParams;

    private SurfaceHolder mSurfaceHolder;
	private long mNativeHandle;

	String dir;

	@Override protected void onCreate( Bundle icicle )
	{
		Log.v( TAG, "----------------------------------------------------------------" );
		Log.v( TAG, "GLES3JNIActivity::onCreate()" );
		super.onCreate( icicle );

        SurfaceView mView = new SurfaceView(this);
		setContentView(mView);
		mView.getHolder().addCallback( this );

		dir = "/sdcard/QuakeQuest";
		//dir = getBaseContext().getExternalFilesDir(null).getAbsolutePath();

		// Force the screen to stay on, rather than letting it dim and shut off
		// while the user is watching a movie.
		getWindow().addFlags( WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON );

		// Force screen brightness to stay at maximum
		WindowManager.LayoutParams params = getWindow().getAttributes();
		params.screenBrightness = 1.0f;
		getWindow().setAttributes( params );

		checkPermissionsAndInitialize();
	}

	private boolean waitingForPermission = false;
	/** Initializes the Activity only if the permission has been granted. */
	private void checkPermissionsAndInitialize() {
		if (!Environment.isExternalStorageManager()) {
			waitingForPermission = true;
			//request for the permission
			Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
			Uri uri = Uri.fromParts("package", getPackageName(), null);
			intent.setData(uri);
			startActivityForResult(intent, 1);
		}
		else
		{
			// Permissions have already been granted.
			create();
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == 1) {
			waitingForPermission = false;
			if (Environment.isExternalStorageManager()) {
				create();
			} else {
				Log.e(TAG, "Permission not granted");
			}
		}
	}

	public void create() {
		//This will copy the shareware version of quake if user doesn't have anything installed
		copy_asset(dir + "/id1", "pak0.pak");
		copy_asset(dir + "/id1", "config.cfg");
		copy_asset(dir, "commandline.txt");

		try {
			setenv("QUAKEQUEST_DIR", dir, true);
		} catch (Exception ignored)
		{
			System.exit(-9);;
		}

		//Read these from a file and pass through
		commandLineParams = new String("quake");

		//See if user is trying to use command line params
		if(new File(dir + "/commandline.txt").exists()) // should exist!
		{
			BufferedReader br;
			try {
				br = new BufferedReader(new FileReader(dir + "/commandline.txt"));
				String s;
				StringBuilder sb=new StringBuilder(0);
				while ((s=br.readLine())!=null)
					sb.append(s + " ");
				br.close();

				commandLineParams = new String(sb.toString());
			} catch (FileNotFoundException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			} catch (IOException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			}
		}

		mNativeHandle = GLES3JNILib.onCreate( this, commandLineParams );
	}
	
	public void copy_asset(String path, String name) {
		File f = new File(path + "/" + name);
		if (!f.exists()) {
			
			//Ensure we have an appropriate folder
			new File(path).mkdirs();
			_copy_asset(name, path + "/" + name);
		}
	}

	public void _copy_asset(String name_in, String name_out) {
		AssetManager assets = this.getAssets();

		try {
			InputStream in = assets.open(name_in);
			OutputStream out = new FileOutputStream(name_out);

			copy_stream(in, out);

			out.close();
			in.close();

		} catch (Exception e) {

			e.printStackTrace();
		}

	}

	public static void copy_stream(InputStream in, OutputStream out)
			throws IOException {
		byte[] buf = new byte[1024];
		while (true) {
			int count = in.read(buf);
			if (count <= 0)
				break;
			out.write(buf, 0, count);
		}
	}

	public void shutdown() {
		System.exit(0);
	}

	@Override protected void onStart() {
		super.onStart();
		if (!waitingForPermission && mNativeHandle != 0) {
			GLES3JNILib.onStart(mNativeHandle, this);
		}
	}

	@Override protected void onResume() {
		super.onResume();
		if (!waitingForPermission && mNativeHandle != 0) {
			GLES3JNILib.onResume(mNativeHandle);
		}
	}

	@Override protected void onPause() {
		if (!waitingForPermission && mNativeHandle != 0) {
			GLES3JNILib.onPause(mNativeHandle);
		}
		super.onPause();
	}

	@Override protected void onStop() {
		if (!waitingForPermission && mNativeHandle != 0) {
			GLES3JNILib.onStop(mNativeHandle);
		}
		super.onStop();
	}

	@Override protected void onDestroy()
	{
		Log.v( TAG, "GLES3JNIActivity::onDestroy()" );

		if ( mSurfaceHolder != null )
		{
			GLES3JNILib.onSurfaceDestroyed( mNativeHandle );
		}

		GLES3JNILib.onDestroy( mNativeHandle );

		super.onDestroy();
		mNativeHandle = 0;
	}

	@Override public void surfaceCreated( SurfaceHolder holder )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceCreated()" );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceCreated( mNativeHandle, holder.getSurface() );
			mSurfaceHolder = holder;
		}
	}

	@Override public void surfaceChanged( SurfaceHolder holder, int format, int width, int height )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceChanged()" );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceChanged( mNativeHandle, holder.getSurface() );
			mSurfaceHolder = holder;
		}
	}
	
	@Override public void surfaceDestroyed( SurfaceHolder holder )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceDestroyed()" );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceDestroyed( mNativeHandle );
			mSurfaceHolder = null;
		}
	}
}
