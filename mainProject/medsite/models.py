# medsite/models.py
from django.db import models
from django.conf import settings
import secrets
from django.utils import timezone


ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no confusing 0/O, 1/I

def generate_public_code(length=8):
    return "PUB-" + "".join(secrets.choice(ALPHABET) for _ in range(length))

class Patient(models.Model):
    doctor = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        on_delete=models.SET_NULL,
        null=True, blank=True,
        related_name="patients",
    )

    name = models.CharField(max_length=120)
    age = models.PositiveIntegerField(null=True, blank=True)
    address = models.TextField(blank=True)
    emergency_number = models.CharField(max_length=30, blank=True)

    public_code = models.CharField(max_length=20, unique=True, blank=True, editable=False)

    

    # ✅ archive
    is_archived = models.BooleanField(default=False, db_index=True)
    archived_at = models.DateTimeField(null=True, blank=True)

    def save(self, *args, **kwargs):
        if not self.public_code:
            while True:
                code = generate_public_code(8)
                if not Patient.objects.filter(public_code=code).exists():
                    self.public_code = code
                    break
        super().save(*args, **kwargs)

    def archive(self):
        self.is_archived = True
        self.archived_at = timezone.now()
        self.save(update_fields=["is_archived", "archived_at"])

    def unarchive(self):
        self.is_archived = False
        self.archived_at = None
        self.save(update_fields=["is_archived", "archived_at"])


class Reading(models.Model):
    patient = models.ForeignKey(Patient, on_delete=models.CASCADE, related_name="readings")
    created_at = models.DateTimeField(auto_now_add=True)

    ir = models.BigIntegerField(null=True, blank=True)
    red = models.BigIntegerField(null=True, blank=True)
    finger = models.BooleanField(default=False)

    bpm = models.IntegerField(null=True, blank=True)
    spo2 = models.FloatField(null=True, blank=True)
    pi = models.FloatField(null=True, blank=True)
    rr = models.FloatField(null=True, blank=True)
    sbp = models.IntegerField(null=True, blank=True)
    dbp = models.IntegerField(null=True, blank=True)
    temp = models.FloatField(null=True, blank=True)
