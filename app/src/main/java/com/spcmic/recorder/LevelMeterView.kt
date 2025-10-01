package com.spcmic.recorder

import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.view.View
import kotlin.math.max
import kotlin.math.min

class LevelMeterView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {
    
    private var channelLevels = FloatArray(84) { 0f }
    private var peakLevels = FloatArray(84) { 0f }
    private var peakHoldTime = LongArray(84) { 0L }
    
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val backgroundPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    
    private val channelWidth = 8f
    private val channelSpacing = 2f
    private val meterHeight = 200f
    private val peakHoldDuration = 1000L // Peak hold for 1 second
    
    init {
        backgroundPaint.color = Color.BLACK
        textPaint.color = Color.WHITE
        textPaint.textSize = 24f
        textPaint.textAlign = Paint.Align.CENTER
    }
    
    fun updateLevels(levels: FloatArray) {
        if (levels.size == 84) {
            val currentTime = System.currentTimeMillis()
            
            for (i in 0 until 84) {
                channelLevels[i] = max(0f, min(1f, levels[i]))
                
                // Update peak levels
                if (channelLevels[i] > peakLevels[i]) {
                    peakLevels[i] = channelLevels[i]
                    peakHoldTime[i] = currentTime
                } else if (currentTime - peakHoldTime[i] > peakHoldDuration) {
                    peakLevels[i] = max(0f, peakLevels[i] - 0.01f)
                }
            }
            
            invalidate()
        }
    }
    
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        
        // Draw background
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), backgroundPaint)
        
        // Calculate layout
        val totalChannels = 84
        val totalWidth = totalChannels * (channelWidth + channelSpacing) - channelSpacing
        val startX = (width - totalWidth) / 2
        val startY = 50f
        
        // Draw title
        canvas.drawText("84-Channel Level Meter", width / 2f, 30f, textPaint)
        
        for (i in 0 until totalChannels) {
            val x = startX + i * (channelWidth + channelSpacing)
            
            // Draw channel background
            paint.color = Color.GRAY
            canvas.drawRect(x, startY, x + channelWidth, startY + meterHeight, paint)
            
            // Draw current level
            val levelHeight = channelLevels[i] * meterHeight
            val levelY = startY + meterHeight - levelHeight
            
            // Color based on level (green -> yellow -> red)
            paint.color = when {
                channelLevels[i] < 0.7f -> Color.GREEN
                channelLevels[i] < 0.9f -> Color.YELLOW
                else -> Color.RED
            }
            
            canvas.drawRect(x, levelY, x + channelWidth, startY + meterHeight, paint)
            
            // Draw peak indicator
            if (peakLevels[i] > 0) {
                val peakY = startY + meterHeight - (peakLevels[i] * meterHeight)
                paint.color = Color.WHITE
                canvas.drawRect(x, peakY - 2f, x + channelWidth, peakY, paint)
            }
            
            // Draw channel number every 10 channels
            if (i % 10 == 0 || i == totalChannels - 1) {
                textPaint.textSize = 16f
                canvas.drawText(
                    "${i + 1}",
                    x + channelWidth / 2,
                    startY + meterHeight + 25f,
                    textPaint
                )
            }
        }
        
        // Draw scale on the right
        textPaint.textSize = 12f
        textPaint.textAlign = Paint.Align.LEFT
        val scaleX = startX + totalWidth + 20f
        
        for (db in listOf(0, -6, -12, -18, -24)) {
            val level = dbToLinear(db.toFloat())
            val y = startY + meterHeight - (level * meterHeight)
            canvas.drawText("${db}dB", scaleX, y + 5f, textPaint)
            
            // Draw scale line
            paint.color = Color.WHITE
            canvas.drawLine(scaleX - 10f, y, scaleX - 5f, y, paint)
        }
    }
    
    private fun dbToLinear(db: Float): Float {
        return Math.pow(10.0, db / 20.0).toFloat()
    }
    
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val desiredWidth = (84 * (channelWidth + channelSpacing) + 100).toInt()
        val desiredHeight = (meterHeight + 100).toInt()
        
        val width = resolveSize(desiredWidth, widthMeasureSpec)
        val height = resolveSize(desiredHeight, heightMeasureSpec)
        
        setMeasuredDimension(width, height)
    }
}