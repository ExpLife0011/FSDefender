// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommon.h"
#include "stdio.h"

HRESULT HrMain();

int main()
{
	HRESULT hr = HrMain();
	if (FAILED(hr))
	{
		printf("Main failed with status 0x%x\n", hr);
		return 1;
	}
	
    return 0;
}

HRESULT HrMain()
{
	HRESULT hr = S_OK;

	CFSDPortConnector aConnector;
	hr = aConnector.Initialize(g_wszFSDPortName);
	RETURN_IF_FAILED(hr);

	LPCSTR szMessage = "Test message";
	DWORD  dwMessageSize = (DWORD)strlen(szMessage);

	printf("Sending message: %s\n", szMessage);

	BYTE pReply[256];
	DWORD dwReplySize = sizeof(pReply);
	hr = aConnector.SendMessage((LPVOID)szMessage, dwMessageSize, pReply, &dwReplySize);
	RETURN_IF_FAILED(hr);

	if (dwReplySize > 0)
	{
		printf("Recieved response: %s", pReply);
	}

	CFSDPortConnectorMessage aMessage = {};
	hr = aConnector.RecieveMessage(&aMessage);
	RETURN_IF_FAILED(hr);

	if (aMessage.aRecieveHeader.ReplyLength)
	{
		printf("New message recieved: %s\n", aMessage.pBuffer);
	}

	return S_OK;
}

