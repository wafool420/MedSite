from django.contrib import admin
from .models import Patient, Reading

@admin.register(Patient)
class PatientAdmin(admin.ModelAdmin):
    readonly_fields = ("public_code",)
    list_display = ("name", "public_code", "doctor", "age", "emergency_number")
    list_filter = ("doctor",)
    search_fields = ("name", "public_code", "doctor__username")

    def save_model(self, request, obj, form, change):
        if not obj.doctor and request.user.is_authenticated:
            obj.doctor = request.user
        super().save_model(request, obj, form, change)

@admin.register(Reading)
class ReadingAdmin(admin.ModelAdmin):
    list_display = ("created_at", "patient", "finger", "bpm", "spo2", "temp")
    ordering = ("-created_at",)
