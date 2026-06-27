package com.example.repro

import android.os.Bundle
import android.util.DisplayMetrics
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import androidx.core.view.updatePaddingRelative
import com.google.androidgamesdk.GameActivity
import kotlin.math.roundToInt

class MainActivity : GameActivity() {
    companion object {
        init {
            System.loadLibrary("repro")
        }
    }

    val Int.dp: Int get() {
        val displayMetrics = resources.displayMetrics
        val px = (this * (displayMetrics.xdpi / DisplayMetrics.DENSITY_DEFAULT)).roundToInt()
        return px
    }

    private lateinit var overlay: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        overlay = TextView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.BOTTOM
            }
            textAlignment = TextView.TEXT_ALIGNMENT_TEXT_START
            updatePaddingRelative(start = 8.dp, end = 8.dp, top = 8.dp, bottom = 32.dp)
        }
        super.onCreate(savedInstanceState)
        val contentView = findViewById<FrameLayout>(contentViewId)
        contentView.addView(overlay)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUi()
        }
    }

    private fun hideSystemUi() {
        val decorView = window.decorView
        decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_FULLSCREEN)
    }
}