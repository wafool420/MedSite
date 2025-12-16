# medsite/views.py
import json
from datetime import timedelta

from django.contrib.auth import login, logout
from django.contrib.auth.decorators import login_required
from django.contrib.auth.forms import AuthenticationForm
from django.http import JsonResponse
from django.shortcuts import get_object_or_404, redirect, render
from django.utils import timezone
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_GET, require_POST
from django.conf import settings
from .forms import PatientForm, RegisterForm
from .models import Patient, Reading
from django.urls import reverse



# ---------------- helpers ----------------
def to_bool(v):
    if isinstance(v, bool):
        return v
    if v is None:
        return False
    if isinstance(v, (int, float)):
        return bool(v)
    if isinstance(v, str):
        return v.strip().lower() in ("1", "true", "t", "yes", "y", "on")
    return False


# ---------------- pages ----------------
def home(request):
    esp32_url = getattr(settings, "ESP32_STATUS_URL", "").strip()

    if request.user.is_authenticated:
        patients = Patient.objects.filter(
            doctor=request.user, is_archived=False
        ).order_by("name")

        archived_patients = Patient.objects.filter(
            doctor=request.user, is_archived=True
        ).order_by("-archived_at", "name")

        return render(request, "medsite/home.html", {
            "patients": patients,
            "archived_patients": archived_patients,
            "esp32_url": esp32_url,   # ✅ add this back
        })

    return render(request, "medsite/home.html", {
        "esp32_url": esp32_url,       # ✅ also for guests
    })

@login_required
@require_POST
def archive_patient(request, patient_id):
    patient = get_object_or_404(Patient, id=patient_id, doctor=request.user, is_archived=False)
    patient.archive()
    return redirect("home")

@login_required
@require_POST
def unarchive_patient(request, patient_id):
    patient = get_object_or_404(Patient, id=patient_id, doctor=request.user, is_archived=True)
    patient.unarchive()
    return redirect("home")


@login_required
def create_patient(request):
    if request.method == "POST":
        form = PatientForm(request.POST)
        if form.is_valid():
            p = form.save(commit=False)
            p.doctor = request.user
            p.save()
            return redirect("home")
    else:
        form = PatientForm()

    return render(request, "medsite/create_patient.html", {"form": form})


@login_required
def patient_detail(request, patient_id):
    patient = get_object_or_404(Patient, id=patient_id, doctor=request.user)
    return render(request, "medsite/patient_detail.html", {"patient": patient})

@require_GET
def public_monitor(request, public_code):
    patient = get_object_or_404(Patient, public_code=public_code)

    # optional: block archived
    if getattr(patient, "is_archived", False):
        return render(request, "medsite/monitor_unavailable.html", {"patient": patient}, status=404)

    endpoint = reverse("api_latest", args=[public_code])
    share_url = request.build_absolute_uri(reverse("public_monitor", args=[public_code]))

    # ✅ show address only to the assigned doctor (logged in + owns patient)
    can_view_private = request.user.is_authenticated and patient.doctor_id == request.user.id

    return render(request, "medsite/stats.html", {
        "patient": patient,
        "endpoint": endpoint,
        "share_url": share_url,
        "can_view_private": can_view_private,
    })

@login_required
def stats_page(request, patient_id):
    patient = get_object_or_404(
        Patient, id=patient_id, doctor=request.user, is_archived=False
    )
    return render(request, "medsite/stats.html", {"patient": patient})



# ---------------- APIs ----------------
@login_required
def api_latest_patient(request, patient_id):
    patient = get_object_or_404(
        Patient, id=patient_id, doctor=request.user, is_archived=False
    )

    r = patient.readings.order_by("-created_at").first()
    if not r or (timezone.now() - r.created_at > timedelta(seconds=5)):
        return JsonResponse({"detail": "Machine unavailable"}, status=200)

    return JsonResponse({
        "created_at": r.created_at.isoformat(),
        "ir": r.ir, "red": r.red, "finger": r.finger,
        "bpm": r.bpm, "spo2": r.spo2, "pi": r.pi, "rr": r.rr,
        "sbp": r.sbp, "dbp": r.dbp, "temp": r.temp,
    })

@require_GET
def api_latest(request, public_code):
    patient = get_object_or_404(Patient, public_code=public_code)

    r = patient.readings.order_by("-created_at").first()
    if not r or (timezone.now() - r.created_at > timedelta(seconds=5)):
        return JsonResponse({"detail": "Machine unavailable"}, status=200)

    return JsonResponse({
        "created_at": r.created_at.isoformat(),
        "ir": r.ir, "red": r.red, "finger": r.finger,
        "bpm": r.bpm, "spo2": r.spo2,
        "sbp": r.sbp, "dbp": r.dbp,
        "temp": r.temp,
    })

@csrf_exempt
@require_POST
def api_ingest(request):
    code = request.headers.get("X-PUBLIC-CODE", "").strip()
    if not code:
        return JsonResponse({"detail": "Missing X-PUBLIC-CODE"}, status=400)

    try:
        patient = Patient.objects.get(public_code=code)
        if patient.is_archived:
            return JsonResponse({"detail": "Patient is archived"}, status=403)
    except Patient.DoesNotExist:
        return JsonResponse({"detail": "Invalid patient code"}, status=403)

    try:
        payload = json.loads(request.body.decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("JSON must be an object")
    except Exception:
        return JsonResponse({"detail": "Invalid JSON"}, status=400)

    r = Reading.objects.create(
        patient=patient,
        ir=payload.get("ir"),
        red=payload.get("red"),
        finger=to_bool(payload.get("finger", False)),
        bpm=payload.get("bpm"),
        spo2=payload.get("spo2"),
        pi=payload.get("pi"),
        rr=payload.get("rr"),
        sbp=payload.get("sbp"),
        dbp=payload.get("dbp"),
        temp=payload.get("temp"),
    )

    return JsonResponse({"ok": True, "id": r.id})


# (Optional) keep your old public-code latest endpoint for debugging only.
# If you don't use it anymore, you can delete it.



# ---------------- auth ----------------
def register_view(request):
    if request.user.is_authenticated:
        return redirect("home")

    if request.method == "POST":
        form = RegisterForm(request.POST)
        if form.is_valid():
            user = form.save()
            login(request, user)
            return redirect("home")
    else:
        form = RegisterForm()

    return render(request, "medsite/register.html", {"form": form})


def login_view(request):
    if request.user.is_authenticated:
        return redirect("home")

    form = AuthenticationForm(request, data=request.POST or None)
    if request.method == "POST" and form.is_valid():
        login(request, form.get_user())
        return redirect("home")

    return render(request, "medsite/login.html", {"form": form})


@require_POST
def logout_view(request):
    logout(request)
    return redirect("home")
