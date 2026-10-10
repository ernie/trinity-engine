package io.ernie.trinity;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.RemoteException;
import android.util.Log;
import android.util.Pair;
import android.view.KeyEvent;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.core.content.FileProvider;

import com.drbeef.externalhapticsservice.HapticServiceClient;
import com.drbeef.externalhapticsservice.HapticsConstants;

import org.libsdl.app.SDLActivity;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.util.Locale;
import java.util.Vector;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import static android.system.Os.setenv;

public class MainActivity extends SDLActivity
{
	private boolean nativeReady = false;

	private static final int READ_EXTERNAL_STORAGE_PERMISSION_ID = 1;
	private static final int WRITE_EXTERNAL_STORAGE_PERMISSION_ID = 2;
	private static final int RECORD_AUDIO_PERMISSION_ID = 3;
	private static final int EYE_TRACKING_PERMISSION_ID = 4;
	private static final String TAG = "Trinity";

	private static final String DEFAULT_HOME_DIR = "/sdcard/Trinity";
	// Quake3Quest's folder: the first launch copies from it and never writes there
	private static final String LEGACY_HOME_DIR = "/sdcard/ioquake3Quest";
	// Documents survives reinstalls, and each Steam Frame install gets fresh app storage
	private static final String SHARED_HOME_DIR = "/sdcard/Documents/Trinity";

	private String homeDir = DEFAULT_HOME_DIR;

	private boolean hapticsEnabled = false;

	String commandLineParams;

	private Vector<HapticServiceClient> externalHapticsServiceClients = new Vector<>();

	// one package may offer several haptic services, so not a map
	private static Vector<Pair<String, String>> externalHapticsServiceDetails = new Vector<>();

	// The engine library is the last one listed, so SDLActivity runs its SDL_main
	@Override
	protected String[] getLibraries() {
		return new String[] { "SDL2", "trinity" };
	}

	// SDLActivity loads the native libraries; the engine thread waits in SDL_main until create() hands over
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		Log.i(TAG, "onCreate called");
		super.onCreate(savedInstanceState);
		if (SDLActivity.mBrokenLibraries) {
			return;
		}
		try {
			checkPermissionsAndInitialize();
		} catch (Exception e) {
			Log.e(TAG, "Activity initialization failed", e);
		}
	}

	@Override protected void onDestroy()
	{
		Log.i(TAG, "onDestroy called");

		for (HapticServiceClient externalHapticsServiceClient : externalHapticsServiceClients) {
			externalHapticsServiceClient.stopBinding();
		}

		super.onDestroy();
	}

	private void checkPermissionsAndInitialize() throws IOException {
		if (ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
				!= PackageManager.PERMISSION_GRANTED) {
			ActivityCompat.requestPermissions(this,
					new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,
							Manifest.permission.WRITE_EXTERNAL_STORAGE},
					WRITE_EXTERNAL_STORAGE_PERMISSION_ID);
		} else {
			create();
		}
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
		if (requestCode == WRITE_EXTERNAL_STORAGE_PERMISSION_ID) {
			if (ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
					!= PackageManager.PERMISSION_GRANTED) {
				Log.w(TAG, "Storage permission denied; the game cannot read its data directory");
				finish();
				return;
			}
			try {
				create();
			} catch (Exception e) {
				Log.e(TAG, "Activity initialization failed", e);
			}
		} else if (requestCode == RECORD_AUDIO_PERMISSION_ID) {
			if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
				Log.d(TAG, "Microphone permission granted; VOIP available");
			} else {
				Log.w(TAG, "Microphone permission denied; VOIP unavailable");
			}
		} else if (requestCode == EYE_TRACKING_PERMISSION_ID) {
			for (int i = 0; i < permissions.length && i < results.length; i++) {
				Log.i(TAG, "Eye tracking permission " + permissions[i] + ": "
					+ (results[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "denied"));
			}
		}
	}

	// SDLActivity starts the native thread once its Surface is ready, but PICO never delivers a Surface to a VR activity
	@Override
	protected void onResume() {
		super.onResume();
		startNativeIfReady();
	}

	private void startNativeIfReady() {
		if (!nativeReady || !SDLActivity.mIsResumedCalled || mSurface == null) return;
		if (!mSurface.mIsSurfaceReady) {
			Log.i(TAG, "Starting native thread without waiting for an Android surface");
			mSurface.mIsSurfaceReady = true;
		}
		SDLActivity.handleNativeState();
	}

	public void create() throws IOException {
		if (nativeReady) return;
		homeDir = new File(SHARED_HOME_DIR).isDirectory() ? SHARED_HOME_DIR : DEFAULT_HOME_DIR;
		Log.i(TAG, "Game files: " + homeDir);

		new File(homeDir + "/baseq3").mkdirs();

		migrateLegacyHome();

		copy_asset(homeDir, "cacert.pem", true);

		// Licenses belong where a user can read them, not only inside the APK
		copy_asset(homeDir, "THIRD-PARTY-NOTICES.txt", true);

		copy_asset(homeDir, "commandline.txt", false);
		copy_asset(homeDir + "/baseq3", "autoexec.cfg", false);
		// only Trinity's paks: the retail game data is the player's own
		copy_asset(homeDir + "/baseq3", "pak8t.pk3", true);
		copy_asset(homeDir + "/baseq3", "zzz-trinity-announcer.pk3", true);

		if (new File(homeDir + "/missionpack").exists()) {
			copy_asset(homeDir + "/missionpack", "pak3t.pk3", true);
		}

		commandLineParams = new String();

		if (new File(homeDir + "/commandline.txt").exists()) {
			BufferedReader br;
			try {
				br = new BufferedReader(new FileReader(homeDir + "/commandline.txt"));
				String s;
				StringBuilder sb = new StringBuilder(0);
				while ((s = br.readLine()) != null)
					sb.append(s + " ");
				br.close();

				commandLineParams = new String(sb.toString());
			} catch (FileNotFoundException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}
		}

		// overrides any paths an older commandline.txt sets
		commandLineParams += " +set fs_basepath " + homeDir + "/ +set fs_homepath " + homeDir + "/";

		Log.d(TAG, "setting env");
		try {
			setenv("TRINITY_HOME", homeDir, true);
			setenv("commandline", commandLineParams, true);
		} catch (Exception e) {
			Log.e(TAG, "Activity initialization failed", e);
		}

		for (Pair<String, String> serviceDetail : externalHapticsServiceDetails) {
			HapticServiceClient client = new HapticServiceClient(this, (state, desc) -> {
				Log.v(TAG, "ExternalHapticsService " + serviceDetail.second + ": " + desc);
			}, new Intent(serviceDetail.second)
					.setPackage(serviceDetail.first));

			client.bindService();
			externalHapticsServiceClients.add(client);
		}

		Log.d(TAG, "nativeCreate");
		nativeCreate(this);
		nativeReady = true;

		requestMicrophonePermission();

		// for eye-tracked foveated rendering
		requestEyeTrackingPermission();
		startNativeIfReady();
	}

	// Only ask for the names this device defines, so headsets without eye tracking never see a dialog
	private static final String[] EYE_TRACKING_PERMISSIONS = {
		"horizonos.permission.EYE_TRACKING",
		"com.picovr.permission.EYE_TRACKING",
	};

	public void requestEyeTrackingPermission() {
		Vector<String> missing = new Vector<>();
		for (String permission : EYE_TRACKING_PERMISSIONS) {
			try {
				getPackageManager().getPermissionInfo(permission, 0);
			} catch (PackageManager.NameNotFoundException e) {
				continue;
			}
			if (ContextCompat.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
				missing.add(permission);
			}
		}
		if (!missing.isEmpty()) {
			Log.i(TAG, "Requesting eye tracking permission: " + missing);
			ActivityCompat.requestPermissions(this, missing.toArray(new String[0]), EYE_TRACKING_PERMISSION_ID);
		}
	}

	public boolean hasMicrophonePermission() {
		return ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
				== PackageManager.PERMISSION_GRANTED;
	}

	public void requestMicrophonePermission() {
		if (!hasMicrophonePermission()) {
			ActivityCompat.requestPermissions(this,
					new String[]{Manifest.permission.RECORD_AUDIO},
					RECORD_AUDIO_PERMISSION_ID);
		}
	}

	public void installApk(String apkPath) {
		File apkFile = new File(apkPath);
		Uri apkUri;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
			apkUri = FileProvider.getUriForFile(this,
				getPackageName() + ".fileprovider", apkFile);
		} else {
			apkUri = Uri.fromFile(apkFile);
		}
		Intent intent = new Intent(Intent.ACTION_VIEW);
		intent.setDataAndType(apkUri, "application/vnd.android.package-archive");
		intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
		intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
		startActivity(intent);
	}

	private void migrateLegacyHome() {
		if (new File(homeDir, "baseq3/pak0.pk3").exists() || !new File(LEGACY_HOME_DIR, "baseq3/pak0.pk3").exists()) {
			return;
		}
		Log.i(TAG, "Copying game files from " + LEGACY_HOME_DIR + " to " + homeDir);
		copyLegacyGameDir("baseq3");
		copyLegacyGameDir("missionpack");
	}

	// pakQ3Q.pk3 sorts after pak8t/pak3t and would override them
	private void copyLegacyGameDir(String game) {
		File src = new File(LEGACY_HOME_DIR, game);
		File dst = new File(homeDir, game);
		if (!src.isDirectory()) {
			return;
		}
		dst.mkdirs();
		File[] files = src.listFiles();
		if (files != null) {
			for (File f : files) {
				String name = f.getName();
				if (name.toLowerCase(Locale.ROOT).endsWith(".pk3") && !name.equalsIgnoreCase("pakQ3Q.pk3")) {
					copyLegacyFile(f, new File(dst, name));
				}
			}
		}
		copyLegacyFile(new File(src, "autoexec.cfg"), new File(dst, "autoexec.cfg"));
		copyLegacyFile(new File(src, "q3config.cfg"), new File(dst, "q3config.cfg"));
	}

	// kernel-side copy: the paks are too large for copy_stream's 1 KB buffer
	private void copyLegacyFile(File src, File dst) {
		if (!src.isFile() || dst.exists()) {
			return;
		}
		try (FileChannel in = new FileInputStream(src).getChannel();
			 FileChannel out = new FileOutputStream(dst).getChannel()) {
			long size = in.size();
			for (long pos = 0; pos < size; ) {
				long n = in.transferTo(pos, size - pos, out);
				if (n <= 0) {
					throw new IOException("short copy at " + pos + " of " + size);
				}
				pos += n;
			}
		} catch (IOException e) {
			Log.w(TAG, "Failed to copy " + src + ": " + e.getMessage());
			dst.delete();
		}
	}

	public void copy_asset(String path, String name, boolean force) {
		copy_asset(path, name, name, force);
	}

	public void copy_asset(String path, String name, String newName, boolean force) {
		File f = new File(path + "/" + newName);
		if (!f.exists() || force) {

			String fullname = path + "/" + name;
			String directory = fullname.substring(0, fullname.lastIndexOf("/"));
			new File(directory).mkdirs();
			_copy_asset(name, path + "/" + newName);
		}
	}

	public void delete_asset(String path) {
		File file = new File(path);
		delete_asset(file);
	}

	public void delete_asset(File file) {
		if (!file.exists()) {
			return;
		}
		if (file.isDirectory()) {
			for (File nestedFile : file.listFiles()) {
				delete_asset(nestedFile);
			}
		}
		file.delete();
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

	public static native void nativeCreate(MainActivity thisObject);

	static {
		externalHapticsServiceDetails.add(Pair.create(HapticsConstants.BHAPTICS_PACKAGE, HapticsConstants.BHAPTICS_ACTION_FILTER));
		externalHapticsServiceDetails.add(Pair.create(HapticsConstants.FORCETUBE_PACKAGE, HapticsConstants.FORCETUBE_ACTION_FILTER));
	}

	public void haptic_event(String event, int position, int flags, int intensity, float angle, float yHeight)  {

		boolean areHapticsEnabled = hapticsEnabled;
		for (HapticServiceClient externalHapticsServiceClient : externalHapticsServiceClients) {

			if (externalHapticsServiceClient.hasService()) {
				try {
					if (!areHapticsEnabled)
					{
						externalHapticsServiceClient.getHapticsService().hapticEnable();
						hapticsEnabled = true;
						continue;
					}

					if (event.compareTo("frame_tick") == 0)
					{
						externalHapticsServiceClient.getHapticsService().hapticFrameTick();
					}

					// the services key their patterns by Doom3Quest and RTCWQuest event names
					String app = "Doom3Quest";
					String eventID = event;
					if (event.contains(":"))
					{
						String[] items = event.split(":");
						app = items[0];
						eventID = items[1];
					}
					externalHapticsServiceClient.getHapticsService().hapticEvent(app, eventID, position, flags, intensity, angle, yHeight);
				}
				catch (RemoteException r)
				{
					Log.v(TAG, r.toString());
				}
			}
		}
	}
}
