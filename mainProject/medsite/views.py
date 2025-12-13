from django.conf import settings
from rest_framework.views import APIView
from rest_framework.response import Response
from rest_framework import status
from .models import Reading
from .serializers import ReadingIngestSerializer
from django.http import HttpResponse
from django.shortcuts import render

def home(request):
    return HttpResponse(
        "MedSite is running ✅\n"
        "POST: /api/ingest/\n"
        "GET:  /api/latest/<device_id>/\n"
    )

def dashboard(request, device_id="c3-001"):
    # Just renders the page; JS will fetch the latest reading
    return render(request, "medsite/dashboard.html", {"device_id": "c3-001"})



class IngestReading(APIView):
    authentication_classes = []
    permission_classes = []

    def post(self, request):
        if request.headers.get("X-API-KEY") != settings.DEVICE_API_KEY:
            return Response({"detail": "Unauthorized"}, status=status.HTTP_401_UNAUTHORIZED)

        ser = ReadingIngestSerializer(data=request.data)
        if not ser.is_valid():
            return Response(ser.errors, status=status.HTTP_400_BAD_REQUEST)

        r = ser.save()
        return Response({"ok": True, "id": r.id}, status=status.HTTP_201_CREATED)


class LatestReading(APIView):
    authentication_classes = []
    permission_classes = []

    def get(self, request, device_id):
        r = Reading.objects.filter(device_id=device_id).order_by("-created_at").first()
        if not r:
            return Response({"detail": "No data"}, status=status.HTTP_404_NOT_FOUND)

        return Response({
            "device_id": r.device_id,
            "created_at": r.created_at.isoformat(),
            "ir": r.ir,
            "red": r.red,
            "finger": r.finger,
            "bpm": r.bpm,
            "spo2": r.spo2,
            "pi": r.pi,
            "rr": r.rr,
            "sbp": r.sbp,
            "dbp": r.dbp,
            "temp": r.temp,
        })
