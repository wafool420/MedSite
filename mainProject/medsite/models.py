from django.db import models

class Reading(models.Model):
    device_id = models.CharField(max_length=64, db_index=True)
    created_at = models.DateTimeField(auto_now_add=True)

    ir = models.BigIntegerField()
    red = models.BigIntegerField()
    finger = models.BooleanField(default=False)

    bpm = models.IntegerField(null=True, blank=True)
    spo2 = models.FloatField(null=True, blank=True)
    pi = models.FloatField(null=True, blank=True)
    rr = models.FloatField(null=True, blank=True)
    sbp = models.IntegerField(null=True, blank=True)
    dbp = models.IntegerField(null=True, blank=True)
    temp = models.FloatField(null=True, blank=True)

    def __str__(self):
        return f"{self.device_id} @ {self.created_at}"
