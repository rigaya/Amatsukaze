#pragma once

#pragma once

#include <memory>
#include <deque>

#include "Transcode.hpp"
#include "CudaFilter.h"

class VideoFilter : NonCopyable
{
public:
	VideoFilter();

	VideoFilter* nextFilter;

	virtual void start() = 0;
	virtual void onFrame(std::unique_ptr<av::Frame>&& frame) = 0;
	virtual void finish() = 0;

	void sendFrame(std::unique_ptr<av::Frame>&& frame);
};

class TemporalNRFilter : public VideoFilter
{
public:
	TemporalNRFilter();
	
	void init(int temporalDistance, int threshold, bool interlaced);
	virtual void start();
	virtual void onFrame(std::unique_ptr<av::Frame>&& frame);
	virtual void finish();

private:
	enum { MAX_NFRAMES = 128 };

	std::deque<std::unique_ptr<av::Frame>> frames_;

	bool interlaced_;
	int NFRAMES_;
	int DIFFMAX_;

	std::unique_ptr<av::Frame> TNRFilter(AVFrame** frames, int frameIndex);

	template <typename T>
	T getPixel(AVFrame* frame, int idx, int x, int y) {
		return *((T*)(frame->data[idx] + frame->linesize[idx] * y) + x);
	}

	template <typename T>
	void setPixel(AVFrame* frame, int idx, int x, int y, T v) {
		*((T*)(frame->data[idx] + frame->linesize[idx] * y) + x) = v;
	}

	template <typename T>
	int calcDiff(T Y, T U, T V, T rY, T rU, T rV) {
		return
			std::abs((int)Y - (int)rY) +
			std::abs((int)U - (int)rU) +
			std::abs((int)V - (int)rV);
	}

	template <typename T>
	void filterKernel(AVFrame** frames, AVFrame* dst, bool interlaced, int thresh, float* kernel)
	{
		int mid = NFRAMES_ / 2;
		int width = frames[0]->width;
		int height = frames[0]->height;

		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				int cy = interlaced ? (((y >> 1) & ~1) | (y & 1)) : (y >> 1);
				int cx = x >> 1;

				T Y = getPixel<T>(frames[mid], 0, x, y);
				T U = getPixel<T>(frames[mid], 1, cx, cy);
				T V = getPixel<T>(frames[mid], 2, cx, cy);

				float sumKernel = 0.0f;
				for (int i = 0; i < NFRAMES_; i++) {
					T rY = getPixel<T>(frames[i], 0, x, y);
					T rU = getPixel<T>(frames[i], 1, cx, cy);
					T rV = getPixel<T>(frames[i], 2, cx, cy);

					int diff = calcDiff(Y, U, V, rY, rU, rV);
					if (diff <= thresh) {
						sumKernel += kernel[i];
					}
				}

				float factor = 1.f / sumKernel;

				float dY = 0.5f;
				float dU = 0.5f;
				float dV = 0.5f;
				for (int i = 0; i < NFRAMES_; i++) {
					T rY = getPixel<T>(frames[i], 0, x, y);
					T rU = getPixel<T>(frames[i], 1, cx, cy);
					T rV = getPixel<T>(frames[i], 2, cx, cy);

					int diff = calcDiff(Y, U, V, rY, rU, rV);
					if (diff <= thresh) {
						float coef = kernel[i] * factor;
						dY += coef * rY;
						dU += coef * rU;
						dV += coef * rV;
					}
				}

				setPixel(dst, 0, x, y, (T)dY);

				bool cout = (((x & 1) == 0) && (((interlaced ? (y >> 1) : y) & 1) == 0));
				if (cout) {
					setPixel(dst, 1, cx, cy, (T)dU);
					setPixel(dst, 2, cx, cy, (T)dV);
				}
			}
		}
	}
};

class CudaTemporalNRFilter : public VideoFilter
{
public:
	CudaTemporalNRFilter();

	void init(int temporalDistance, int threshold, int batchSize, int interlaced);
	virtual void start();
	virtual void onFrame(std::unique_ptr<av::Frame>&& frame);
	virtual void finish();
private:
	CudaTNRFilter filter_;
	av::Frame frame_;
	std::deque<std::unique_ptr<av::Frame>> frameQueue_;
};

