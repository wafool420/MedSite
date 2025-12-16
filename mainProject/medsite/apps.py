from django.apps import AppConfig

class MedsiteConfig(AppConfig):
    default_auto_field = "django.db.models.BigAutoField"
    name = "medsite"

    def ready(self):
        pass
