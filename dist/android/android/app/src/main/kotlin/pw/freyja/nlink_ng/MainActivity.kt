package pw.freyja.nlink_ng

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val channelName = "nlink/usb"
    private val actionUsbPermission = "pw.freyja.nlink_ng.USB_PERMISSION"
    private var pendingPermission: MethodChannel.Result? = null
    private var pickResult: MethodChannel.Result? = null
    private var connection: UsbDeviceConnection? = null
    private val pickFilesRequest = 42

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != actionUsbPermission) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            pendingPermission?.success(granted)
            pendingPermission = null
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val filter = IntentFilter(actionUsbPermission)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(usbReceiver, filter)
        }
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "listDevices" -> result.success(listDevices())
                    "requestPermission" -> {
                        val id = call.argument<Int>("deviceId")
                        if (id == null) result.error("bad_args", "deviceId required", null)
                        else requestPermission(id, result)
                    }
                    "open" -> {
                        val id = call.argument<Int>("deviceId")
                        if (id == null) result.error("bad_args", "deviceId required", null)
                        else result.success(openDevice(id))
                    }
                    "close" -> {
                        closeDevice()
                        result.success(null)
                    }
                    "pickFiles" -> pickFiles(result)
                    "downloadDir" -> {
                        val dir = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS)
                        result.success(dir?.absolutePath ?: cacheDir.absolutePath)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    override fun onDestroy() {
        closeDevice()
        try {
            unregisterReceiver(usbReceiver)
        } catch (_: Exception) {
        }
        super.onDestroy()
    }

    private fun usbManager(): UsbManager = getSystemService(USB_SERVICE) as UsbManager

    private fun listDevices(): List<Map<String, Any>> {
        val manager = usbManager()
        return manager.deviceList.values.map { device ->
            val nspire = isNspire(device)
            mapOf(
                "deviceId" to device.deviceId,
                "vendorId" to device.vendorId,
                "productId" to device.productId,
                "name" to (device.productName ?: device.deviceName),
                "isNspire" to nspire,
                "model" to nspireModel(device),
                "hasPermission" to manager.hasPermission(device),
            )
        }
    }

    private fun isNspire(device: UsbDevice): Boolean =
        device.vendorId == 0x0451 &&
            (device.productId == 0xe012 || device.productId == 0xe022)

    private fun nspireModel(device: UsbDevice): String {
        if (!isNspire(device)) return ""
        return if (device.productId == 0xe022) "TI-Nspire CX II" else "TI-Nspire"
    }

    private fun requestPermission(deviceId: Int, result: MethodChannel.Result) {
        val manager = usbManager()
        val device = manager.deviceList.values.firstOrNull { it.deviceId == deviceId }
        if (device == null) {
            result.error("not_found", "USB device $deviceId gone", null)
            return
        }
        if (manager.hasPermission(device)) {
            result.success(true)
            return
        }
        pendingPermission?.success(false)
        pendingPermission = result
        val flags =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                PendingIntent.FLAG_MUTABLE
            } else {
                0
            }
        val pi = PendingIntent.getBroadcast(
            this,
            0,
            Intent(actionUsbPermission).setPackage(packageName),
            flags,
        )
        manager.requestPermission(device, pi)
    }

    private fun openDevice(deviceId: Int): Map<String, Any> {
        closeDevice()
        val manager = usbManager()
        val device = manager.deviceList.values.firstOrNull { it.deviceId == deviceId }
            ?: throw RuntimeException("USB device gone")
        if (!manager.hasPermission(device)) {
            throw RuntimeException("USB permission not granted")
        }
        val conn = manager.openDevice(device) ?: throw RuntimeException("openDevice failed")
        val iface = device.getInterface(0)
        if (!conn.claimInterface(iface, true)) {
            conn.close()
            throw RuntimeException("claimInterface failed")
        }
        var epIn = 0
        var epOut = 0
        for (i in 0 until iface.endpointCount) {
            val ep = iface.getEndpoint(i)
            if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
            if (ep.direction == UsbConstants.USB_DIR_IN && epIn == 0) {
                epIn = ep.address
            } else if (ep.direction == UsbConstants.USB_DIR_OUT && epOut == 0) {
                epOut = ep.address
            }
        }
        if (epIn == 0 || epOut == 0) {
            conn.releaseInterface(iface)
            conn.close()
            throw RuntimeException("missing bulk endpoints")
        }
        connection = conn
        return mapOf(
            "fd" to conn.fileDescriptor,
            "epIn" to epIn,
            "epOut" to epOut,
            "isCx2" to (device.productId == 0xe022),
        )
    }

    private fun pickFiles(result: MethodChannel.Result) {
        pickResult?.success(emptyList<String>())
        pickResult = result
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(intent, pickFilesRequest)
    }

    @Deprecated("FlutterActivity still uses this for document picks")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pickFilesRequest) return
        val out = mutableListOf<String>()
        if (resultCode == RESULT_OK && data != null) {
            val clip = data.clipData
            if (clip != null) {
                for (i in 0 until clip.itemCount) {
                    copyUriToCache(clip.getItemAt(i).uri)?.let { out.add(it) }
                }
            } else {
                data.data?.let { copyUriToCache(it)?.let { p -> out.add(p) } }
            }
        }
        pickResult?.success(out)
        pickResult = null
    }

    private fun copyUriToCache(uri: android.net.Uri): String? {
        val name = contentResolver.query(uri, null, null, null, null)?.use { c ->
            val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (c.moveToFirst() && idx >= 0) c.getString(idx) else "upload.bin"
        } ?: "upload.bin"
        val dest = java.io.File(cacheDir, "upl_${System.currentTimeMillis()}_$name")
        return try {
            contentResolver.openInputStream(uri)?.use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
            dest.absolutePath
        } catch (_: Exception) {
            null
        }
    }

    private fun closeDevice() {
        try {
            connection?.close()
        } catch (_: Exception) {
        }
        connection = null
    }
}
