// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  QR code scanner activity for Google Cardboard codes.
 * @author Simon Zeni <simon.zeni@collabora.com>
 */

package org.freedesktop.monado.android_common

import android.Manifest
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.util.Log
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.ImageAnalysis
import androidx.camera.mlkit.vision.MlKitAnalyzer
import androidx.camera.view.LifecycleCameraController
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.mlkit.vision.barcode.BarcodeScanner
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import org.freedesktop.monado.android_common.databinding.ActivityScannerBinding

class QrScannerActivity : AppCompatActivity() {
    private lateinit var viewBinding: ActivityScannerBinding
    private lateinit var barcodeScanner: BarcodeScanner
    private lateinit var qrExecutor: ExecutorService
    private lateinit var qrHandler: Handler
    private var inProgress: Boolean = false

    companion object {
        private val TAG = "MonadoQrScanner"
        private const val PERMISSION_REQUEST_CODE = 200
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        viewBinding = ActivityScannerBinding.inflate(layoutInflater)
        setContentView(viewBinding.root)

        qrExecutor = Executors.newSingleThreadExecutor()
        qrHandler = Handler(Looper.getMainLooper())

        if (
            ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        ) {
            Log.i(TAG, "Camera permission has been granted")
            startCamera()
        } else {
            Log.i(TAG, "Camera permission has not been granted, requesting")
            val permissionStrings = arrayOf(Manifest.permission.CAMERA)
            ActivityCompat.requestPermissions(this, permissionStrings, PERMISSION_REQUEST_CODE)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        qrExecutor.shutdown()
        barcodeScanner.close()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Log.i(TAG, "onRequestPermissionResult")
        if (requestCode == PERMISSION_REQUEST_CODE) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                startCamera()
            }
        }
    }

    private fun startCamera() {
        Log.d(TAG, "Starting Camera")

        val cameraController = LifecycleCameraController(baseContext)
        val previewView = viewBinding.qrPreview

        val options =
            BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        barcodeScanner = BarcodeScanning.getClient(options)

        cameraController.setImageAnalysisAnalyzer(
            ContextCompat.getMainExecutor(this),
            MlKitAnalyzer(
                listOf(barcodeScanner),
                ImageAnalysis.COORDINATE_SYSTEM_VIEW_REFERENCED,
                ContextCompat.getMainExecutor(this),
            ) { result: MlKitAnalyzer.Result? ->
                val barcode =
                    result?.getValue(barcodeScanner)?.firstOrNull() ?: return@MlKitAnalyzer

                when (barcode.valueType) {
                    Barcode.TYPE_URL -> {
                        if (!inProgress) {
                            inProgress = true
                            qrExecutor.execute {
                                val status: UriStatus = qrProcess(barcode)
                                qrHandler.post { qrSave(status) }
                            }
                        }
                    }
                    else -> {
                        Log.i(TAG, "Unsupported data type: ${barcode.rawValue}")
                    }
                }
            },
        )
        cameraController.setImageAnalysisBackpressureStrategy(
            ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST
        )
        cameraController.bindToLifecycle(this)
        previewView.controller = cameraController
    }

    class UriStatus private constructor(val statusCode: Int, val params: ByteArray?) {
        companion object {
            const val OK: Int = 0
            const val FORMAT_ERROR: Int = 1
            const val CONNECTION_ERROR: Int = 2

            fun success(params: ByteArray?): UriStatus {
                return UriStatus(OK, params)
            }

            fun error(statusCode: Int): UriStatus {
                return UriStatus(statusCode, null)
            }
        }
    }

    private fun qrProcess(param: Barcode): UriStatus {
        var uri = Uri.parse(param.rawValue)
        if (!uri.scheme.equals("https")) {
            uri = Uri.parse(uri.toString().replaceFirst("http", "https"))
        }

        Log.i(TAG, "QR code URL ${uri.toString()}")
        var redirect = false
        do {
            val connection = URL(uri.toString()).openConnection() as HttpURLConnection
            connection.setRequestMethod("HEAD")
            connection.connect()
            val code = connection.getResponseCode()
            if (
                code == HttpURLConnection.HTTP_MOVED_PERM ||
                    code == HttpURLConnection.HTTP_MOVED_TEMP
            ) {
                redirect = true
                uri = Uri.parse(connection.getHeaderField("Location"))
                if (!uri.scheme.equals("https")) {
                    uri = Uri.parse(uri.toString().replaceFirst("http", "https"))
                }
            } else {
                redirect = false
            }
            Log.i(TAG, "followed url to ${uri.toString()}")
        } while (redirect)

        Log.i(TAG, "ending with url ${uri.toString()}")

        if (!uri.authority.equals("google.com") || !uri.path.equals("/cardboard/cfg")) {
            Log.e(TAG, "url is not cardboard config")
            return UriStatus.error(UriStatus.FORMAT_ERROR)
        }

        val paramEncoded = uri.getQueryParameter("p")
        val flags = Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING
        return UriStatus.success(Base64.decode(paramEncoded, flags))
    }

    private fun qrSave(result: UriStatus) {
        when (result.statusCode) {
            UriStatus.FORMAT_ERROR -> {
                Log.d(TAG, "QR code invalid")
                Toast.makeText(this, "QR code invalid", Toast.LENGTH_SHORT).show()
            }
            UriStatus.CONNECTION_ERROR -> {
                Log.d(TAG, "Connection error")
                Toast.makeText(this, "Connection error", Toast.LENGTH_SHORT).show()
            }
            UriStatus.OK -> {
                // Log.d(TAG, "Writing parameters to external storage");
                // Can't create 'Cardboard' folder
                // val configFolder = File(Environment.getExternalStorageDirectory(), "Documents")

                val configFile = File(filesDir, "current_device_params")
                configFile.createNewFile()

                Log.d(TAG, "Saving to file $configFile")
                FileOutputStream(configFile).use { it.write(result.params) }

                Log.d(TAG, "QR code valid")
                Toast.makeText(this, "Cardboard parameters saved", Toast.LENGTH_SHORT).show()
            }
        }

        finish()
    }
}
