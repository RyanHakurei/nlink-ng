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
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val channelName = "nlink/usb"
    private val actionUsbPermission = "pw.freyja.nlink_ng.USB_PERMISSION"
    private var pendingPermission: MethodChannel.Result? = null
    private var pickResult: MethodChannel.Result? = null
    private var pickRequest: Int = 0
    private var connection: UsbDeviceConnection? = null
    private var linkEpIn: Int = 0
    private var linkEpOut: Int = 0
    private var claimedInterface: android.hardware.usb.UsbInterface? = null
    private var channel: MethodChannel? = null
    private val pickFilesRequest = 42
    private val pickSaveRequest = 43
    private val pickTreeRequest = 44

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                actionUsbPermission -> {
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    pendingPermission?.success(granted)
                    pendingPermission = null
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    Handler(Looper.getMainLooper()).post {
                        channel?.invokeMethod("usbDetached", null)
                    }
                }
            }
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val permFilter = IntentFilter(actionUsbPermission)
        val detachFilter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, permFilter, RECEIVER_NOT_EXPORTED)
            registerReceiver(usbReceiver, detachFilter, RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(usbReceiver, permFilter)
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(usbReceiver, detachFilter)
        }
        channel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
        channel!!.setMethodCallHandler { call, result ->
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
                    "currentLink" -> {
                        val conn = connection
                        if (conn == null || linkEpIn == 0 || linkEpOut == 0) {
                            result.success(null)
                        } else {
                            result.success(
                                mapOf(
                                    "fd" to conn.fileDescriptor,
                                    "epIn" to linkEpIn,
                                    "epOut" to linkEpOut,
                                ),
                            )
                        }
                    }
                    "pickFiles" -> pickFiles(result)
                    "pickSaveFile" -> pickSaveFile(call.argument<String>("name") ?: "download.bin", result)
                    "pickSaveTree" -> pickSaveTree(result)
                    "cacheDir" -> result.success(cacheDir.absolutePath)
                    "copyToUri" -> {
                        val src = call.argument<String>("src")
                        val uri = call.argument<String>("uri")
                        if (src == null || uri == null) {
                            result.error("bad_args", "src and uri required", null)
                        } else {
                            try {
                                copyToUri(src, uri)
                                result.success(null)
                            } catch (e: Exception) {
                                result.error("copy_failed", e.message, null)
                            }
                        }
                    }
                    "copyDirToTree" -> {
                        val src = call.argument<String>("src")
                        val uri = call.argument<String>("uri")
                        val name = call.argument<String>("name")
                        if (src == null || uri == null) {
                            result.error("bad_args", "src and uri required", null)
                        } else {
                            try {
                                copyDirToTree(src, uri, name)
                                result.success(null)
                            } catch (e: Exception) {
                                result.error("copy_failed", e.message, null)
                            }
                        }
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
            val supported = isSupported(device)
            mapOf(
                "deviceId" to device.deviceId,
                "vendorId" to device.vendorId,
                "productId" to device.productId,
                "name" to (device.productName ?: device.deviceName),
                "isNspire" to supported,
                "model" to calculatorModel(device),
                "hasPermission" to manager.hasPermission(device),
            )
        }
    }

    private fun isSupported(device: UsbDevice): Boolean =
        device.vendorId == 0x0451 && device.productId in setOf(
            0xe001, 0xe003, 0xe008, 0xe012, 0xe018, 0xe022,
        )

    private fun calculatorModel(device: UsbDevice): String = when (device.productId) {
        0xe022 -> "TI-Nspire CX II"
        0xe012 -> "TI-Nspire"
        0xe001 -> "SilverLink"
        0xe003 -> "TI-84 Plus"
        0xe008 -> "TI-84 Plus / CE"
        0xe018 -> "TI-84 Evo"
        else -> ""
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
        var claimed: android.hardware.usb.UsbInterface? = null
        var epIn = 0
        var epOut = 0
        for (ifaceIndex in 0 until device.interfaceCount) {
            val iface = device.getInterface(ifaceIndex)
            if (!conn.claimInterface(iface, true)) continue
            var inAddr = 0
            var outAddr = 0
            for (i in 0 until iface.endpointCount) {
                val ep = iface.getEndpoint(i)
                if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (ep.direction == UsbConstants.USB_DIR_IN && inAddr == 0) {
                    inAddr = ep.address
                } else if (ep.direction == UsbConstants.USB_DIR_OUT && outAddr == 0) {
                    outAddr = ep.address
                }
            }
            if (inAddr != 0 && outAddr != 0) {
                claimed = iface
                epIn = inAddr
                epOut = outAddr
                break
            }
            conn.releaseInterface(iface)
        }
        if (claimed == null || epIn == 0 || epOut == 0) {
            conn.close()
            throw RuntimeException("missing bulk endpoints")
        }
        connection = conn
        claimedInterface = claimed
        linkEpIn = epIn
        linkEpOut = epOut
        return mapOf(
            "fd" to conn.fileDescriptor,
            "epIn" to epIn,
            "epOut" to epOut,
            "isCx2" to (device.productId == 0xe022),
            "productId" to device.productId,
        )
    }

    private fun beginPick(result: MethodChannel.Result, request: Int) {
        pickResult?.success(null)
        pickResult = result
        pickRequest = request
    }

    private fun pickFiles(result: MethodChannel.Result) {
        beginPick(result, pickFilesRequest)
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(intent, pickFilesRequest)
    }

    private fun pickSaveFile(name: String, result: MethodChannel.Result) {
        beginPick(result, pickSaveRequest)
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_TITLE, name)
            addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivityForResult(intent, pickSaveRequest)
    }

    private fun pickSaveTree(result: MethodChannel.Result) {
        beginPick(result, pickTreeRequest)
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(intent, pickTreeRequest)
    }

    @Deprecated("FlutterActivity still uses this for document picks")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pickRequest) return
        val pending = pickResult
        pickResult = null
        pickRequest = 0
        if (pending == null) return
        if (resultCode != RESULT_OK || data == null) {
            pending.success(null)
            return
        }
        when (requestCode) {
            pickFilesRequest -> {
                val out = mutableListOf<String>()
                val clip = data.clipData
                if (clip != null) {
                    for (i in 0 until clip.itemCount) {
                        copyUriToCache(clip.getItemAt(i).uri)?.let { out.add(it) }
                    }
                } else {
                    data.data?.let { copyUriToCache(it)?.let { p -> out.add(p) } }
                }
                pending.success(out)
            }
            pickSaveRequest -> {
                val uri = data.data
                if (uri == null) {
                    pending.success(null)
                } else {
                    try {
                        contentResolver.takePersistableUriPermission(
                            uri,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                        )
                    } catch (_: Exception) {
                    }
                    pending.success(uri.toString())
                }
            }
            pickTreeRequest -> {
                val uri = data.data
                if (uri == null) {
                    pending.success(null)
                } else {
                    try {
                        contentResolver.takePersistableUriPermission(
                            uri,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
                        )
                    } catch (_: Exception) {
                    }
                    pending.success(uri.toString())
                }
            }
            else -> pending.success(null)
        }
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

    private fun mimeFor(name: String): String {
        val lower = name.lowercase()
        return when {
            lower.endsWith(".png") -> "image/png"
            lower.endsWith(".jpg") || lower.endsWith(".jpeg") -> "image/jpeg"
            lower.endsWith(".pdf") -> "application/pdf"
            lower.endsWith(".txt") -> "text/plain"
            else -> "application/octet-stream"
        }
    }

    private fun copyToUri(srcPath: String, uriStr: String) {
        val src = java.io.File(srcPath)
        if (!src.isFile) {
            throw RuntimeException("Downloaded file missing")
        }
        val uri = android.net.Uri.parse(uriStr)
        val out = contentResolver.openOutputStream(uri)
            ?: throw RuntimeException("Could not write to the chosen location")
        out.use { output ->
            src.inputStream().use { input -> input.copyTo(output) }
        }
        src.delete()
    }

    private fun treeDocumentUri(tree: android.net.Uri): android.net.Uri {
        val docId = DocumentsContract.getTreeDocumentId(tree)
        return DocumentsContract.buildDocumentUriUsingTree(tree, docId)
    }

    private fun copyDirToTree(srcPath: String, treeUriStr: String, folderName: String?) {
        val src = java.io.File(srcPath)
        if (!src.isDirectory) {
            throw RuntimeException("Downloaded folder missing")
        }
        val tree = android.net.Uri.parse(treeUriStr)
        var parent = treeDocumentUri(tree)
        val name = folderName?.takeIf { it.isNotEmpty() }
        if (name != null) {
            parent = DocumentsContract.createDocument(
                contentResolver,
                parent,
                DocumentsContract.Document.MIME_TYPE_DIR,
                name,
            ) ?: throw RuntimeException("Could not create folder $name")
        }
        copyDirRecursive(src, parent)
        src.deleteRecursively()
    }

    private fun copyDirRecursive(dir: java.io.File, parent: android.net.Uri) {
        val children = dir.listFiles() ?: return
        for (child in children) {
            if (child.isDirectory) {
                val created = DocumentsContract.createDocument(
                    contentResolver,
                    parent,
                    DocumentsContract.Document.MIME_TYPE_DIR,
                    child.name,
                ) ?: throw RuntimeException("Could not create folder ${child.name}")
                copyDirRecursive(child, created)
            } else {
                val created = DocumentsContract.createDocument(
                    contentResolver,
                    parent,
                    mimeFor(child.name),
                    child.name,
                ) ?: throw RuntimeException("Could not create ${child.name}")
                val out = contentResolver.openOutputStream(created)
                    ?: throw RuntimeException("Could not write ${child.name}")
                out.use { output ->
                    child.inputStream().use { input -> input.copyTo(output) }
                }
            }
        }
    }

    private fun closeDevice() {
        try {
            claimedInterface?.let { connection?.releaseInterface(it) }
        } catch (_: Exception) {
        }
        try {
            connection?.close()
        } catch (_: Exception) {
        }
        claimedInterface = null
        connection = null
        linkEpIn = 0
        linkEpOut = 0
    }
}
