#include <comdef.h>
#include <stdexcept>
#include <iostream>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class com_exception : public std::exception {
public:
	com_exception(HRESULT hr)
		: result(hr)
	{}

	const char* what() const override {
		static char s_str[1024] = {};
		sprintf_s(s_str, ">>> Failure with HRESULT of %08X, error message: %ls\n",
			static_cast<unsigned int>(result), _com_error(result).ErrorMessage());
		return s_str;
	}

private:
	HRESULT result;
};

void ThrowIfFailed(HRESULT hr) {
	if (FAILED(hr)) {
		if (IsDebuggerPresent()) {
			__debugbreak();
		}
		throw com_exception(hr);
	}
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

bool ShouldQuit() {
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0u, 0u, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
		if (msg.message == WM_QUIT) {
			return true;
		}
	}
	return false;
}

void Run() {
	unsigned int const width = 800;
	unsigned int const height = 600;

	HINSTANCE hInstance = GetModuleHandle(nullptr);
	WNDCLASSEX wndClass{
		sizeof(WNDCLASSEX),
		0u,
		WindowProc,
		0,
		0,
		hInstance,
		nullptr,
		LoadCursor(nullptr, IDC_ARROW),
		nullptr,
		nullptr,
		L"MainWindowClass",
		nullptr
	};
	DWORD exStyle = WS_EX_OVERLAPPEDWINDOW;
	DWORD style = WS_OVERLAPPEDWINDOW;
	RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	AdjustWindowRectEx(&rect, style, FALSE, exStyle);
	RegisterClassEx(&wndClass);
	HWND hwnd = CreateWindowEx(
		exStyle,
		L"MainWindowClass",
		L"MainWindow",
		style,
		5, 5,
		rect.right - rect.left,
		rect.bottom - rect.top,
		nullptr,
		nullptr,
		hInstance,
		nullptr
	);
	if (hwnd == nullptr) {
		throw std::runtime_error("Window creation failed");
	}
	ShowWindow(hwnd, SW_SHOWDEFAULT);

	ComPtr<ID3D12Debug> debugController;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
	debugController->EnableDebugLayer();

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));

	// choose adapter w/ max video memory
	ComPtr<IDXGIAdapter1> adapter = nullptr;
	DXGI_ADAPTER_DESC adapterDesc;
	ComPtr<IDXGIAdapter1> pCurAdapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &pCurAdapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC curAdapterDesc;
		ThrowIfFailed(pCurAdapter->GetDesc(&curAdapterDesc));
		if (adapter == nullptr || curAdapterDesc.DedicatedVideoMemory > adapterDesc.DedicatedVideoMemory) {
			adapter = pCurAdapter;
			adapterDesc = curAdapterDesc;
		}
	}
	std::wcout << "Picked " << adapterDesc.Description << std::endl;
	ComPtr<ID3D12Device> device;
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

	D3D12_COMMAND_QUEUE_DESC queueDesc;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.NodeMask = 0u;
	ComPtr<ID3D12CommandQueue> queue;
	ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));

	ComPtr<ID3D12Resource> backbuffers[2];
	DXGI_SWAP_CHAIN_DESC1 swapchainDesc;
	swapchainDesc.Width = width;
	swapchainDesc.Height = height;
	swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapchainDesc.Stereo = FALSE;
	swapchainDesc.SampleDesc = { 1, 0 };
	swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapchainDesc.BufferCount = (uint32_t)std::size(backbuffers);
	swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	swapchainDesc.Flags = 0u;
	ComPtr<IDXGISwapChain1> swapchain1;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &swapchainDesc,
		nullptr, nullptr, &swapchain1));
	ThrowIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
	ComPtr<IDXGISwapChain3> swapchain;
	ThrowIfFailed(swapchain1.As(&swapchain));

	D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc;
	descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	descHeapDesc.NumDescriptors = (uint32_t)std::size(backbuffers);
	descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	descHeapDesc.NodeMask = 0u;
	ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
	ThrowIfFailed(device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&rtvDescHeap)));

	size_t rtvHeapBase = rtvDescHeap->GetCPUDescriptorHandleForHeapStart().ptr;
	uint32_t descRtvSz = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	ComPtr<ID3D12CommandAllocator> commandAllocators[std::size(backbuffers)];
	for (uint32_t i = 0; i < std::size(backbuffers); i++) {
		ThrowIfFailed(swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])));
		D3D12_CPU_DESCRIPTOR_HANDLE descHandle{ rtvHeapBase + (size_t)descRtvSz * i };
		device->CreateRenderTargetView(backbuffers[i].Get(), nullptr, descHandle);

		ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&commandAllocators[i])));
	}

	ComPtr<ID3D12GraphicsCommandList> commandList;
	ThrowIfFailed(device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0].Get(),
		nullptr, IID_PPV_ARGS(&commandList)));
	ThrowIfFailed(commandList->Close());

	uint64_t fenceValue = 0;
	ComPtr<ID3D12Fence> fence;
	ThrowIfFailed(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	while (!ShouldQuit()) {
		uint32_t curFrame = swapchain->GetCurrentBackBufferIndex();
		ThrowIfFailed(commandAllocators[curFrame]->Reset());
		commandList->Reset(commandAllocators[curFrame].Get(), nullptr);

		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = backbuffers[curFrame].Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		commandList->ResourceBarrier(1u, &barrier);
		
		D3D12_CPU_DESCRIPTOR_HANDLE rt{ rtvHeapBase + (size_t)descRtvSz * curFrame };
		FLOAT clearColor[4] = { 0.7f, 1.0f, 0.7f, 1.0f };
		commandList->ClearRenderTargetView(rt, clearColor, 0u, nullptr);
		commandList->OMSetRenderTargets(1u, &rt, FALSE, nullptr);

		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		commandList->ResourceBarrier(1u, &barrier);

		ThrowIfFailed(commandList->Close());
		ID3D12CommandList* cl = commandList.Get();
		queue->ExecuteCommandLists(1u, &cl);

		ThrowIfFailed(swapchain->Present(1, 0)); // d3d12SDKLayers crash here
		
		ThrowIfFailed(queue->Signal(fence.Get(), ++fenceValue));
		ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, nullptr));
	}
}

int main() {
	try {
		Run();
	}	catch (std::exception& e) {
		std::cerr << e.what() << std::endl;
	}	catch (...) {
		std::cerr << "Unknown error" << std::endl;
	}
	return 0;
}